#include "audio.h"
#include "stretch.h"

#include <3ds.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#define MINIMP3_IMPLEMENTATION
#include "../third_party/minimp3/minimp3.h"
#include "../third_party/faad2/neaacdec.h"
#include "../third_party/faad2/frontend/mp4read.h"

#define AUDIO_CHANNEL 0
#define INPUT_BYTES 16384
/* Keep complete MP3 frames while holding two 52 ms chunks in reserve.  This
   gives NDSP enough headroom for 3DS UI frames and SD I/O; a single 26 ms
   chunk starved the DSP and made audio sound slow and broken. */
#define PCM_FRAMES 2304
#define AAC_PENDING_FRAMES 4096
#define SCRATCH_CACHE_FRAMES 16384
#define SCRATCH_GRAIN_FRAMES 512
#define MP3_SEEK_POINTS 16384
#define MP3_SEEK_STEP_MS 50
#define HOT_CUE_SLOTS 8
/* Slots 9–18 are reserved for Beat Jump.  They deliberately never evict a
   user's A–H pads, so both operations can stay on the direct-PCM path. */
#define PERFORMANCE_CACHE_SLOTS 18
#define HOT_CUE_CACHE_FRAMES 48000
#define LOOP_CACHE_FRAMES 48000
#define LOOP_CACHE_MS 1000
/* A Hot Cue must cover the SD-card reopen and decoder warm-up too.  One beat
   was only 300 ms on fast tracks, so uncached tracks still stalled at the
   first buffer boundary.  This is source time, not output time. */
#define HOT_CUE_CACHE_MS 1000

static FILE *stream;
static mp3dec_t decoder;
static NeAACDecHandle aac_decoder;
static u8 input[INPUT_BYTES];
static size_t input_size;
static size_t input_offset;
static s16 *pcm[2];
static s16 *scratch_cache;
static s16 *hot_cue_cache;
static s16 *loop_cache;
static s16 *stretch_input;
static ndspWaveBuf wavebuf[2];
/* Hot Cue PCM is linear memory, so NDSP can read the complete cached second
   directly.  Keeping it separate from the two streaming buffers means SD
   I/O cannot starve the first sound after a pad press. */
static ndspWaveBuf hotcue_wavebuf;
static ndspWaveBuf loop_wavebuf;
static bool initialized;
/* Azahar can run the 3DS application and render Citro2D correctly while its
   NDSP service is unavailable.  Keep decoding/UI testable in that case, but
   never issue channel commands to a DSP that did not initialise. */
static bool ndsp_available;
static bool loaded;
static bool source_finished;
static bool is_aac;
static u32 sample_rate = 44100;
/* Source-time transport is advanced from buffers NDSP has actually consumed.
   The UI reads this rather than trusting an unrelated frame/UI clock. */
static u64 wavebuf_start_samples[2], wavebuf_end_samples[2];
static bool wavebuf_source_known[2];
/* MT output is generated ahead of NDSP. Keep the source-time ratio that was
   active when each queued SoundTouch buffer was produced; a newly moved tempo
   fader must not advance the waveform through already-rendered old-tempo PCM. */
static float wavebuf_source_rate[2];
static u64 transport_position_samples, transport_anchor_tick;
static bool transport_running;
/* Source-time carried by the PCM that has actually left SoundTouch. Unlike
   decoder position, this remains continuous across its internal overlap and
   is the only valid clock for the MT waveform. */
static double mt_output_source_samples;
static float tempo_percent;
static float pitch_bend_percent;
static bool master_tempo_enabled;
static s16 aac_pending[AAC_PENDING_FRAMES * 2];
static int aac_pending_count;
static int aac_pending_offset;
static u32 aac_decode_cursor_ms;
static u64 aac_source_position_samples;
static int aac_decode_error_streak;
static u32 stream_duration_ms;
static u32 mp3_seek_offsets[MP3_SEEK_POINTS];
static u16 mp3_seek_skips[MP3_SEEK_POINTS];
static u32 mp3_seek_count;
static u32 mp3_initial_skip_samples;
static u32 aac_initial_skip_samples;
static u32 gapless_delay_samples;
static u32 mp3_source_sample_rate = 44100;
static char current_path[512];
static const char *status = "AUDIO OFF";
static bool scrub_preview;
static bool reverse_first_buffer;
static float scrub_speed = 1.0f;
static bool scratch_capture, scratch_active;
static int captured_frames[2], scratch_cache_frames;
static u32 scratch_cache_start_ms;
static float scratch_position_frames, scratch_rate_frames;
static bool scratch_stationary;
/* The ordinary PCM producer continuously retains a small source-time window.
   A touch can therefore enter scratch immediately without doing a filesystem
   seek or decoding a second of AAC on the UI thread. */
static bool live_scratch_valid;
static int live_scratch_frames;
static u64 live_scratch_start_samples;
static bool hot_cue_cached[PERFORMANCE_CACHE_SLOTS];
static bool preserving_hot_cue_cache;
static u8 hot_cue_numbers[PERFORMANCE_CACHE_SLOTS];
static u32 hot_cue_times[PERFORMANCE_CACHE_SLOTS];
static int hot_cue_cache_frames[PERFORMANCE_CACHE_SLOTS];
static u32 hot_cue_duration_ms[PERFORMANCE_CACHE_SLOTS];
static u64 hot_cue_start_samples[PERFORMANCE_CACHE_SLOTS], hot_cue_resume_samples[PERFORMANCE_CACHE_SLOTS];
static bool loop_enabled;
static u32 loop_start_ms, loop_end_ms;
static u64 source_position_samples, loop_start_samples, loop_end_samples;
static bool loop_cache_ready, loop_transition_requested, loop_cache_playing;
static bool loop_resume_pending, loop_source_queued, loop_source_prepared;
static bool loop_transport_restarted;
static bool loop_clock_active;
static u64 loop_clock_tick;
/* The PCM buffer handed to NDSP must retain its own immutable source range.
   The user can change x2/x1/2 while that buffer is still being played; using
   the newly selected bounds for the old buffer made its clock jump and, more
   importantly, allowed its PCM to be overwritten mid-output. */
static u64 loop_buffer_start_samples, loop_buffer_end_samples;
static u64 loop_buffer_resume_samples;
static int loop_cache_frames;
/* A loop cache is valid only for the exact Loop-In source coordinate that
   produced it. Length alone is not an identity: two different Cue regions
   can have the same number of beats. */
static u64 loop_cache_start_samples = (u64)-1;
/* First loop pass is captured directly from the ordinary PCM producer.  This
   avoids decoding seconds of audio synchronously when the user hits a pad. */
static bool loop_capture_active;
static int loop_capture_frames, loop_capture_target_frames, loop_capture_output_frames;
static u64 loop_capture_next_samples;
static u32 loop_cache_duration_ms, loop_resume_ms;
static u64 loop_resume_samples;
static u64 loop_resume_prepare_after;
static bool hotcue_resume_pending;
static bool hotcue_resume_fallback;
static u32 hotcue_resume_ms;
static u64 hotcue_start_samples, hotcue_resume_samples, hotcue_clock_tick;
static bool hotcue_clock_active;
static u64 hotcue_resume_prepare_after;
static bool hotcue_cache_playing;
static bool hotcue_source_queued;
static const s16 *hotcue_cache_source;
static int hotcue_cache_total_frames, hotcue_cache_next_frame;

static bool audio_seek_internal(u32 milliseconds, bool fast, bool reverse, bool scrub, float speed);
static bool prepare_mp3_source(u32 milliseconds, bool reset_channel, bool fast_reset,
                               bool preserve_stretch);
static bool prepare_m4a_source(u32 milliseconds, bool reset_channel, bool fast_reset,
                               bool preserve_stretch);
static void begin_loop_cache_transition(void);
static bool begin_master_loop_cache_transition(void);
static bool needs_master_tempo_processing(void);

/* The container indexes in the cache are milliseconds, but the PCM renderer
   and DSP operate in samples.  Retain the remaining sub-millisecond offset
   after a seek so a loop cannot accumulate a rounding error at every wrap. */
static void align_mp3_source_samples(u64 exact_samples, u32 prepared_ms) {
    const u64 prepared_samples = (u64)prepared_ms * sample_rate / 1000u;
    if (exact_samples > prepared_samples) {
        const u64 extra = exact_samples - prepared_samples;
        if (extra <= 0xffffffffu - mp3_initial_skip_samples)
            mp3_initial_skip_samples += (u32)extra;
    }
    source_position_samples = exact_samples;
}

static void align_aac_source_samples(u64 exact_samples, u32 prepared_ms) {
    const u64 prepared_samples = (u64)prepared_ms * sample_rate / 1000u;
    if (exact_samples > prepared_samples) {
        const u64 extra = exact_samples - prepared_samples;
        if (extra <= 0xffffffffu - aac_initial_skip_samples)
            aac_initial_skip_samples += (u32)extra;
    }
    aac_source_position_samples = exact_samples;
}

static void reset_transport(u64 source_samples) {
    transport_position_samples = source_samples;
    transport_anchor_tick = osGetTime();
}

static u64 current_transport_samples(void) {
    if (loop_clock_active) {
        float rate = 1.0f + tempo_percent / 100.0f;
        rate *= 1.0f + pitch_bend_percent / 100.0f;
        if (rate < 0.025f) rate = 0.025f;
        const u64 loop_samples = loop_buffer_end_samples - loop_buffer_start_samples;
        if (loop_samples) {
            const u64 elapsed = (u64)llround((double)(osGetTime() - loop_clock_tick) *
                                              sample_rate * rate / 1000.0);
            return loop_buffer_start_samples + elapsed % loop_samples;
        }
    }
    if (hotcue_clock_active) {
        float rate = 1.0f + tempo_percent / 100.0f;
        rate *= 1.0f + pitch_bend_percent / 100.0f;
        if (rate < 0.025f) rate = 0.025f;
        return hotcue_start_samples +
            (u64)llround((double)(osGetTime() - hotcue_clock_tick) * sample_rate * rate / 1000.0);
    }
    u64 samples = transport_position_samples;
    if (transport_running) {
        float rate = 1.0f + tempo_percent / 100.0f;
        if (needs_master_tempo_processing()) {
            for (int i = 0; i < 2; ++i) {
                if (wavebuf_source_known[i] && wavebuf[i].status == NDSP_WBUF_PLAYING) {
                    rate = wavebuf_source_rate[i];
                    break;
                }
            }
        }
        rate *= 1.0f + pitch_bend_percent / 100.0f;
        if (rate < 0.025f) rate = 0.025f;
        samples += (u64)llround((double)(osGetTime() - transport_anchor_tick) * sample_rate * rate / 1000.0);
    }
    return samples;
}

static void mark_completed_wavebuf(int index) {
    if (index < 0 || index > 1 || !wavebuf_source_known[index]) return;
    transport_position_samples = wavebuf_end_samples[index];
    transport_anchor_tick = osGetTime();
    wavebuf_source_known[index] = false;
}

static void apply_output_rate(void) {
    if (!ndsp_available) return;
    float factor = master_tempo_enabled ? 1.0f : 1.0f + tempo_percent / 100.0f;
    factor *= 1.0f + pitch_bend_percent / 100.0f;
    if (factor < 0.025f) factor = 0.025f; /* WIDE's -100% endpoint stays in the same timeline basis. */
    if (scrub_preview) factor *= scrub_speed;
    const float rate = (float)sample_rate * factor;
    ndspChnSetRate(AUDIO_CHANNEL, rate);
}

/* At exactly 0.00% Master Tempo has nothing to correct.  Sending PCM through
   SoundTouch anyway adds interpolation/overlap artefacts, heard as a gritty
   texture even though the requested speed is unchanged. */
static bool needs_master_tempo_processing(void) {
    return master_tempo_enabled && fabsf(tempo_percent) >= 0.005f;
}

/* loop_cache contains native-rate decoded PCM.  It can only be submitted
   directly when the output path is also native-rate; otherwise it bypasses
   SoundTouch and is guaranteed to disagree with the active MT tempo. */
static bool can_use_direct_loop_cache(void) {
    return loop_cache_ready && !needs_master_tempo_processing();
}

/* The same cached PCM remains useful with MT, but only as SoundTouch input:
   it must never be submitted straight to NDSP. */
static bool can_use_master_loop_cache(void) {
    return loop_cache_ready && needs_master_tempo_processing() && loop_cache_frames > 0;
}

/* ndspChnReset clears the channel's rate and format as well as its queued
   buffers.  Every cue seek performs a reset, so restoring all three settings
   here is essential: otherwise Auto Cue starts at the DSP's reset rate and
   sounds dramatically slow. */
static void reset_channel_for_seek(bool fast) {
    if (!ndsp_available) return;
    ndspChnSetPaused(AUDIO_CHANNEL, true);
    ndspChnReset(AUDIO_CHANNEL);
    /* A performance seek never reuses a buffer after reset, so a short DSP
       hand-off is sufficient.  Full seeks retain the conservative wait used
       for track loading and Cue release. */
    /* Loop/Hot-Cue starts bind freshly reset buffers, so they need only a
       sub-millisecond DSP hand-off.  A full 20 ms is retained for loads and
       release seeks where UI responsiveness is not performance-critical. */
    svcSleepThread((fast ? 500 : 20 * 1000) * 1000LL);
    ndspChnSetInterp(AUDIO_CHANNEL, NDSP_INTERP_LINEAR);
    ndspChnSetFormat(AUDIO_CHANNEL, NDSP_FORMAT_STEREO_PCM16);
    wavebuf_source_known[0] = wavebuf_source_known[1] = false;
    transport_running = false;
    apply_output_rate();
}

static void refill_input(void) {
    if (!stream || source_finished) return;
    if (input_offset > 0 && input_offset < input_size)
        memmove(input, input + input_offset, input_size - input_offset);
    if (input_offset > 0) input_size -= input_offset;
    input_offset = 0;
    const size_t read = fread(input + input_size, 1, INPUT_BYTES - input_size, stream);
    input_size += read;
    if (read == 0) source_finished = true;
}

static void append_live_scratch(const s16 *source, int frames, u64 source_start) {
    if (!scratch_cache || !source || frames <= 0 || scratch_capture || scratch_active) return;
    if (frames >= SCRATCH_CACHE_FRAMES) {
        memcpy(scratch_cache, source + (frames - SCRATCH_CACHE_FRAMES) * 2,
               SCRATCH_CACHE_FRAMES * 2 * sizeof(s16));
        live_scratch_start_samples = source_start + (u64)(frames - SCRATCH_CACHE_FRAMES);
        live_scratch_frames = SCRATCH_CACHE_FRAMES;
        live_scratch_valid = true;
        return;
    }
    if (!live_scratch_valid || source_start != live_scratch_start_samples + (u64)live_scratch_frames) {
        memcpy(scratch_cache, source, (size_t)frames * 2 * sizeof(s16));
        live_scratch_start_samples = source_start;
        live_scratch_frames = frames;
        live_scratch_valid = true;
        return;
    }
    const int keep = live_scratch_frames + frames > SCRATCH_CACHE_FRAMES ?
        SCRATCH_CACHE_FRAMES - frames : live_scratch_frames;
    if (keep > 0 && live_scratch_frames > keep)
        memmove(scratch_cache, scratch_cache + (live_scratch_frames - keep) * 2,
                (size_t)keep * 2 * sizeof(s16));
    memcpy(scratch_cache + keep * 2, source, (size_t)frames * 2 * sizeof(s16));
    live_scratch_start_samples = source_start + (u64)(live_scratch_frames - keep);
    live_scratch_frames = keep + frames;
    live_scratch_valid = true;
}

static void append_loop_capture(const s16 *source, int frames, u64 source_start) {
    if (!loop_capture_active || !loop_cache || !source || frames <= 0) return;
    const u64 source_end = source_start + (u64)frames;
    const u64 capture_end = loop_start_samples + (u64)loop_capture_target_frames;
    const u64 begin = source_start > loop_start_samples ? source_start : loop_start_samples;
    const u64 end = source_end < capture_end ? source_end : capture_end;
    if (end <= begin || begin != loop_capture_next_samples) return;
    const int source_offset = (int)(begin - source_start);
    const int count = (int)(end - begin);
    memcpy(loop_cache + loop_capture_frames * 2, source + source_offset * 2,
           (size_t)count * 2 * sizeof(s16));
    loop_capture_frames += count;
    loop_capture_next_samples = end;
    if (loop_capture_frames < loop_capture_target_frames) return;
    /* Repeat a short loop into a one-second direct buffer. This still gives
       the continuation decoder ample warm-up time, while halving the linear
       cache flush at Loop In (the source of the audible boundary hitch). */
    for (int frame = loop_capture_frames; frame < loop_capture_output_frames; ++frame) {
        const int source_frame = frame % loop_capture_frames;
        loop_cache[frame * 2] = loop_cache[source_frame * 2];
        loop_cache[frame * 2 + 1] = loop_cache[source_frame * 2 + 1];
    }
    loop_cache_frames = loop_capture_output_frames;
    loop_cache_duration_ms = (u32)((u64)loop_cache_frames * 1000u / sample_rate);
    const u64 loop_samples = loop_end_samples - loop_start_samples;
    loop_resume_samples = loop_start_samples + ((u64)loop_cache_frames % loop_samples);
    loop_resume_ms = (u32)(loop_resume_samples * 1000u / sample_rate);
    loop_cache_start_samples = loop_start_samples;
    loop_cache_ready = true;
    loop_capture_active = false;
}

/* The rolling producer cache normally contains the playhead.  Directly after
   a load, seek or cue it can be just ahead of it, though the two NDSP buffers
   still hold the exact audible samples.  Copy those before resetting NDSP so
   the first scratch grain is always immediately available. */
static bool snapshot_queued_scratch(u64 target) {
    int containing = -1;
    for (int i = 0; i < 2; ++i) {
        if (wavebuf_source_known[i] && target >= wavebuf_start_samples[i] &&
            target < wavebuf_end_samples[i]) {
            containing = i;
            break;
        }
    }
    if (containing < 0 || !pcm[containing]) return false;
    int order[2] = { containing, containing ^ 1 };
    const int other = order[1];
    /* Preserve a preceding chunk when it is contiguous, giving a little
       reverse-scratch headroom without waiting for a decoder. */
    if (wavebuf_source_known[other] &&
        wavebuf_end_samples[other] == wavebuf_start_samples[containing]) {
        order[0] = other;
        order[1] = containing;
    }
    int written = 0;
    u64 expected = 0;
    for (int n = 0; n < 2; ++n) {
        const int index = order[n];
        if (!wavebuf_source_known[index] || !pcm[index]) continue;
        if (written && wavebuf_start_samples[index] != expected) continue;
        const u64 source_frames = wavebuf_end_samples[index] - wavebuf_start_samples[index];
        const int frames = source_frames > PCM_FRAMES ? PCM_FRAMES : (int)source_frames;
        if (frames <= 0) continue;
        if (!written) live_scratch_start_samples = wavebuf_start_samples[index];
        memcpy(scratch_cache + written * 2, pcm[index], (size_t)frames * 2 * sizeof(s16));
        written += frames;
        expected = wavebuf_end_samples[index];
    }
    if (!written || target < live_scratch_start_samples ||
        target >= live_scratch_start_samples + (u64)written) return false;
    live_scratch_frames = written;
    live_scratch_valid = true;
    return true;
}

static void queue_wavebuf(int index, int frames_written, u64 source_start, bool source_known) {
    if (reverse_first_buffer) {
        /* The 2deck player moves a scratch reader in either direction.  The
           3DS MP3/AAC decoders are forward-only, so reverse the freshly
           decoded short PCM grain before handing it to NDSP. */
        for (int left = 0, right = frames_written - 1; left < right; ++left, --right) {
            const s16 l = pcm[index][left * 2], r = pcm[index][left * 2 + 1];
            pcm[index][left * 2] = pcm[index][right * 2];
            pcm[index][left * 2 + 1] = pcm[index][right * 2 + 1];
            pcm[index][right * 2] = l;
            pcm[index][right * 2 + 1] = r;
        }
        reverse_first_buffer = false;
    }
    if (scratch_capture) {
        captured_frames[index] = frames_written;
        return;
    }
    wavebuf_start_samples[index] = source_start;
    wavebuf_end_samples[index] = source_start + (u64)frames_written;
    wavebuf_source_known[index] = source_known;
    wavebuf_source_rate[index] = 1.0f + tempo_percent / 100.0f;
    if (source_known) {
        append_live_scratch(pcm[index], frames_written, source_start);
        append_loop_capture(pcm[index], frames_written, source_start);
    }
    if (!ndsp_available) return;
    const size_t bytes = (size_t)frames_written * 2 * sizeof(s16);
    DSP_FlushDataCache(pcm[index], bytes);
    memset(&wavebuf[index], 0, sizeof(wavebuf[index]));
    wavebuf[index].data_vaddr = pcm[index];
    wavebuf[index].nsamples = (u32)frames_written;
    ndspChnWaveBufAdd(AUDIO_CHANNEL, &wavebuf[index]);
}

static void mark_mt_wavebuf_transport(int index, int frames_written) {
    if (index < 0 || index > 1 || frames_written <= 0) return;
    const u64 start = (u64)floor(mt_output_source_samples);
    float factor = 1.0f + tempo_percent / 100.0f;
    if (factor < 0.025f) factor = 0.025f;
    mt_output_source_samples += (double)frames_written * factor;
    wavebuf_start_samples[index] = start;
    wavebuf_end_samples[index] = (u64)floor(mt_output_source_samples);
    if (wavebuf_end_samples[index] <= start)
        wavebuf_end_samples[index] = start + 1;
    wavebuf_source_known[index] = true;
    wavebuf_source_rate[index] = factor;
}

static bool queue_scratch_wavebuf(int index) {
    if (!scratch_active || scratch_stationary || !scratch_cache || scratch_cache_frames <= 0) return false;
    int frames = 0;
    while (frames < SCRATCH_GRAIN_FRAMES) {
        const int source = (int)floorf(scratch_position_frames);
        if (source < 0 || source >= scratch_cache_frames) break;
        pcm[index][frames * 2] = scratch_cache[source * 2];
        pcm[index][frames * 2 + 1] = scratch_cache[source * 2 + 1];
        scratch_position_frames += scratch_rate_frames;
        ++frames;
    }
    if (frames <= 0) return false;
    const size_t bytes = (size_t)frames * 2 * sizeof(s16);
    DSP_FlushDataCache(pcm[index], bytes);
    memset(&wavebuf[index], 0, sizeof(wavebuf[index]));
    wavebuf[index].data_vaddr = pcm[index];
    wavebuf[index].nsamples = (u32)frames;
    if (ndsp_available) ndspChnWaveBufAdd(AUDIO_CHANNEL, &wavebuf[index]);
    return true;
}

static int decode_mp3_frames(s16 *output, int maximum) {
    if (!loaded || !output || maximum <= 0) return 0;
    int frames_written = 0;
    while (frames_written < maximum) {
        /* This happens in the PCM producer, before NDSP receives a sample
           beyond the endpoint.  It is therefore independent of frame rate,
           waveform rendering, and the application's visual timeline. */
        if (loop_enabled && source_position_samples >= loop_end_samples) {
            if (can_use_direct_loop_cache()) {
                loop_transition_requested = true;
                return frames_written;
            }
            if (can_use_master_loop_cache()) {
                if (!begin_master_loop_cache_transition()) return false;
                continue;
            }
            /* Keep SoundTouch's overlap/history alive at the loop boundary.
               Resetting it here created an output starvation gap whenever MT
               was active, even though the decoded source loop was exact. */
            if (!prepare_mp3_source(loop_start_ms, false, false, true)) return false;
            continue;
        }
        if (input_size - input_offset < 2048 && !source_finished) refill_input();
        if (input_size <= input_offset) break;
        mp3d_sample_t frame_pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
        mp3dec_frame_info_t info;
        const int samples = mp3dec_decode_frame(&decoder, input + input_offset,
            (int)(input_size - input_offset), frame_pcm, &info);
        if (info.frame_bytes <= 0) {
            ++input_offset; /* Skip ID3 bytes or a damaged byte while seeking sync. */
            continue;
        }
        input_offset += (size_t)info.frame_bytes;
        if (samples <= 0 || info.channels <= 0) continue;
        if (info.hz > 0 && info.hz != (int)sample_rate) {
            sample_rate = (u32)info.hz;
            if (master_tempo_enabled) stretch_reset(sample_rate, 1.0f + tempo_percent / 100.0f);
            apply_output_rate();
        }
        int source_frame = 0;
        if (mp3_initial_skip_samples) {
            if (mp3_initial_skip_samples >= (u32)samples) {
                mp3_initial_skip_samples -= (u32)samples;
                continue;
            }
            source_frame = (int)mp3_initial_skip_samples;
            mp3_initial_skip_samples = 0;
        }
        while (source_frame < samples && frames_written < maximum) {
            if (loop_enabled && source_position_samples >= loop_end_samples) {
                if (can_use_direct_loop_cache()) {
                    loop_transition_requested = true;
                    return frames_written;
                }
                if (can_use_master_loop_cache()) {
                    if (!begin_master_loop_cache_transition()) return frames_written;
                    break;
                }
                break;
            }
            const int source = source_frame * info.channels;
            const s16 left = frame_pcm[source];
            const s16 right = info.channels > 1 ? frame_pcm[source + 1] : left;
            output[frames_written * 2] = left;
            output[frames_written * 2 + 1] = right;
            ++source_frame;
            ++frames_written;
            ++source_position_samples;
        }
        /* PCM_FRAMES is a multiple of 1152 (and 576 for MPEG-2), so a decoded
           MP3 frame is never split or discarded between output buffers. */
    }
    return frames_written;
}

static bool fill_mp3_wavebuf(int index) {
    if (!loaded || !pcm[index]) return false;
    if (!needs_master_tempo_processing()) {
        const u64 source_start = source_position_samples;
        const int frames = decode_mp3_frames(pcm[index], PCM_FRAMES);
        if (frames > 0) {
            queue_wavebuf(index, frames, source_start, true);
            /* Capturing the final sample can make the cache ready only after
               queue_wavebuf. Queue the direct Loop PCM in this same producer
               pass, rather than waiting for another 2304-frame buffer. */
            if (loop_enabled && source_position_samples >= loop_end_samples && can_use_direct_loop_cache())
                loop_transition_requested = true;
        }
        if (loop_transition_requested) begin_loop_cache_transition();
        return frames > 0 || loop_cache_playing;
    }
    int frames_written = 0;
    while (frames_written < PCM_FRAMES) {
        const u32 received = stretch_receive(pcm[index] + frames_written * 2,
                                             (u32)(PCM_FRAMES - frames_written));
        if (received) { frames_written += (int)received; continue; }
        const int input_frames = decode_mp3_frames(stretch_input, PCM_FRAMES);
        if (input_frames <= 0) break;
        stretch_put(stretch_input, (u32)input_frames);
    }
    if (frames_written <= 0) return false;
    queue_wavebuf(index, frames_written, 0, false);
    mark_mt_wavebuf_transport(index, frames_written);
    return true;
}

static bool decode_next_aac_frame(void) {
    if (!aac_decoder || mp4read_frame() != 0) {
        source_finished = true;
        return false;
    }

    NeAACDecFrameInfo info;
    memset(&info, 0, sizeof(info));
    s16 *decoded = (s16 *)NeAACDecDecode(aac_decoder, &info,
        mp4config.bitbuf.data, mp4config.bitbuf.size);
    /* mp4read_frame already advanced to the next access unit.  Do not remove
       a bad packet from time: that compressed the audio timeline and made all
       later waveform/cue coordinates drift.  Conceal it as one silent AAC
       access unit, then let the next valid unit resume normally. */
    if (info.error || !decoded || !info.samples || !info.channels) {
        const int frames = (info.samples && info.channels) ?
            (int)(info.samples / info.channels) : 1024;
        const int safe_frames = frames > 0 && frames <= AAC_PENDING_FRAMES ? frames : 1024;
        memset(aac_pending, 0, (size_t)safe_frames * 2 * sizeof(s16));
        aac_pending_count = safe_frames;
        aac_pending_offset = 0;
        /* Do not turn a recoverable run of malformed access units into an
           end-of-track condition.  Some rekordbox exports contain a longer
           encoder-priming region; mp4read_frame() is the sole authoritative
           end marker.  Keeping timeline-sized silence here also preserves
           every later beat, cue and waveform coordinate. */
        ++aac_decode_error_streak;
        return true;
    }
    aac_decode_error_streak = 0;
    if (info.samplerate && info.samplerate != sample_rate) {
        sample_rate = info.samplerate;
        apply_output_rate();
    }

    const int frames = (int)(info.samples / info.channels);
    if (frames <= 0 || frames > AAC_PENDING_FRAMES) return false;
    for (int frame = 0; frame < frames; ++frame) {
        const int source = frame * info.channels;
        aac_pending[frame * 2] = decoded[source];
        aac_pending[frame * 2 + 1] = info.channels > 1 ? decoded[source + 1] : decoded[source];
    }
    aac_pending_count = frames;
    aac_pending_offset = 0;
    aac_decode_cursor_ms += (u32)((u64)frames * 1000 / sample_rate);
    return true;
}

/* Seeking AAC requires a small decoder history.  An individual damaged access
   unit is recoverable (the container cursor has already advanced), so do not
   abort a Hot Cue continuation just because its preroll crossed one. */
static bool prime_aac_to_frame(u32 target_frame) {
    int skipped = 0;
    while (mp4config.frame.current < target_frame) {
        aac_pending_count = aac_pending_offset = 0;
        if (decode_next_aac_frame()) { skipped = 0; continue; }
        if (source_finished || ++skipped >= 32) return false;
    }
    return true;
}

static int decode_m4a_frames(s16 *output, int maximum) {
    if (!loaded || !output || maximum <= 0) return 0;
    int frames_written = 0;
    while (frames_written < maximum) {
        if (loop_enabled && aac_source_position_samples >= loop_end_samples) {
            if (can_use_direct_loop_cache()) {
                loop_transition_requested = true;
                return frames_written;
            }
            if (can_use_master_loop_cache()) {
                if (!begin_master_loop_cache_transition()) return frames_written;
                continue;
            }
            /* The first loop pass may reach its end one producer call before
               its captured PCM is marked ready. Keep producing from Loop In
               instead of returning an empty NDSP buffer at Loop Out. */
            if (!prepare_m4a_source(loop_start_ms, false, true, true)) return frames_written;
            continue;
        }
        if (aac_pending_offset >= aac_pending_count) {
            aac_pending_offset = aac_pending_count = 0;
            if (!decode_next_aac_frame()) {
                if (source_finished) break;
                continue; /* Bad AAC access unit: try the following one. */
            }
        }
        const int available = aac_pending_count - aac_pending_offset;
        if (aac_initial_skip_samples) {
            if (aac_initial_skip_samples >= (u32)available) {
                aac_initial_skip_samples -= (u32)available;
                aac_pending_offset = aac_pending_count;
                continue;
            }
            aac_pending_offset += (int)aac_initial_skip_samples;
            aac_initial_skip_samples = 0;
        }
        const int adjusted_available = aac_pending_count - aac_pending_offset;
        const int wanted = maximum - frames_written;
        int take = adjusted_available < wanted ? adjusted_available : wanted;
        if (loop_enabled && aac_source_position_samples + (u64)take > loop_end_samples)
            take = (int)(loop_end_samples - aac_source_position_samples);
        if (take <= 0) continue;
        memcpy(output + frames_written * 2,
               aac_pending + aac_pending_offset * 2,
               (size_t)take * 2 * sizeof(s16));
        frames_written += take;
        aac_pending_offset += take;
        aac_source_position_samples += (u64)take;
    }
    return frames_written;
}

static bool fill_m4a_wavebuf(int index) {
    if (!loaded || !pcm[index]) return false;
    if (!needs_master_tempo_processing()) {
        const u64 source_start = aac_source_position_samples;
        const int frames = decode_m4a_frames(pcm[index], PCM_FRAMES);
        if (frames <= 0) return false;
        queue_wavebuf(index, frames, source_start, true);
        if (loop_enabled && aac_source_position_samples >= loop_end_samples && can_use_direct_loop_cache())
            loop_transition_requested = true;
        if (loop_transition_requested) begin_loop_cache_transition();
        return true;
    }
    int frames_written = 0;
    while (frames_written < PCM_FRAMES) {
        const u32 received = stretch_receive(pcm[index] + frames_written * 2,
                                             (u32)(PCM_FRAMES - frames_written));
        if (received) { frames_written += (int)received; continue; }
        const int input_frames = decode_m4a_frames(stretch_input, PCM_FRAMES);
        if (input_frames <= 0) break;
        stretch_put(stretch_input, (u32)input_frames);
    }
    if (frames_written <= 0) return false;
    queue_wavebuf(index, frames_written, 0, false);
    mark_mt_wavebuf_transport(index, frames_written);
    if (loop_transition_requested) begin_loop_cache_transition();
    return true;
}

static bool fill_wavebuf(int index) {
    return is_aac ? fill_m4a_wavebuf(index) : fill_mp3_wavebuf(index);
}

static bool fill_scratch_cache(u32 centre_ms) {
    if (!scratch_cache || !loaded) return false;
    /* A short, immediate PCM window is enough for finger motion.  Expanding
       eight seconds here blocked the UI at touch-down and touch-up. */
    const u32 start_ms = centre_ms > 180 ? centre_ms - 180 : 0;
    scratch_active = false;
    scratch_capture = true;
    captured_frames[0] = captured_frames[1] = 0;
    if (!audio_seek_internal(start_ms, false, false, false, 1.0f)) {
        scratch_capture = false;
        return false;
    }
    int written = 0;
    for (int i = 0; i < 2 && written < SCRATCH_CACHE_FRAMES; ++i) {
        const int count = captured_frames[i];
        if (count <= 0) continue;
        const int copy = count < SCRATCH_CACHE_FRAMES - written ? count : SCRATCH_CACHE_FRAMES - written;
        memcpy(scratch_cache + written * 2, pcm[i], (size_t)copy * 2 * sizeof(s16));
        written += copy;
    }
    for (int index = 0; written < SCRATCH_CACHE_FRAMES; index ^= 1) {
        captured_frames[index] = 0;
        if (!fill_wavebuf(index) || captured_frames[index] <= 0) break;
        const int count = captured_frames[index];
        const int copy = count < SCRATCH_CACHE_FRAMES - written ? count : SCRATCH_CACHE_FRAMES - written;
        memcpy(scratch_cache + written * 2, pcm[index], (size_t)copy * 2 * sizeof(s16));
        written += copy;
    }
    scratch_capture = false;
    scratch_cache_start_ms = start_ms;
    scratch_cache_frames = written;
    return written > 0;
}

bool audio_init(void) {
    if (initialized) return true;
    ndsp_available = ndspInit() == 0;
    if (ndsp_available) {
        ndspSetOutputMode(NDSP_OUTPUT_STEREO);
        ndspChnReset(AUDIO_CHANNEL);
        ndspChnSetInterp(AUDIO_CHANNEL, NDSP_INTERP_LINEAR);
        ndspChnSetFormat(AUDIO_CHANNEL, NDSP_FORMAT_STEREO_PCM16);
    }
    pcm[0] = linearAlloc(PCM_FRAMES * 2 * sizeof(s16));
    pcm[1] = linearAlloc(PCM_FRAMES * 2 * sizeof(s16));
    scratch_cache = linearAlloc(SCRATCH_CACHE_FRAMES * 2 * sizeof(s16));
    hot_cue_cache = linearAlloc(PERFORMANCE_CACHE_SLOTS * HOT_CUE_CACHE_FRAMES * 2 * sizeof(s16));
    loop_cache = linearAlloc(LOOP_CACHE_FRAMES * 2 * sizeof(s16));
    stretch_input = linearAlloc(PCM_FRAMES * 2 * sizeof(s16));
    if (!pcm[0] || !pcm[1] || !scratch_cache || !hot_cue_cache || !loop_cache || !stretch_input) {
        if (pcm[0]) linearFree(pcm[0]);
        if (pcm[1]) linearFree(pcm[1]);
        if (scratch_cache) linearFree(scratch_cache);
        if (hot_cue_cache) linearFree(hot_cue_cache);
        if (loop_cache) linearFree(loop_cache);
        if (stretch_input) linearFree(stretch_input);
        if (ndsp_available) ndspExit();
        status = "PCM MEMORY ERROR";
        return false;
    }
    initialized = true;
    status = ndsp_available ? "AUDIO READY" : "EMULATOR SILENT";
    return true;
}

bool audio_load_mp3(const char *path) {
    if (!initialized) { status = "NDSP NOT READY"; return false; }
    if (!path || !path[0]) { status = "MP3 PATH ERROR"; return false; }
    audio_stop();
    if (!preserving_hot_cue_cache) memset(hot_cue_cached, 0, sizeof(hot_cue_cached));
    stream = fopen(path, "rb");
    if (!stream) { status = "MP3 OPEN ERROR"; return false; }
    mp3dec_init(&decoder);
    input_size = input_offset = 0;
    source_finished = false;
    mp3_initial_skip_samples = gapless_delay_samples;
    source_position_samples = 0;
    reset_transport(0);
    mt_output_source_samples = 0.0;
    loop_enabled = false;
    is_aac = false;
    sample_rate = 44100;
    if (master_tempo_enabled) stretch_reset(sample_rate, 1.0f + tempo_percent / 100.0f);
    if (ndsp_available) {
        ndspChnReset(AUDIO_CHANNEL);
        ndspChnSetInterp(AUDIO_CHANNEL, NDSP_INTERP_LINEAR);
        ndspChnSetFormat(AUDIO_CHANNEL, NDSP_FORMAT_STEREO_PCM16);
        /* ndspChnReset may clear the paused state.  Keep the new channel
           muted while its first two decoded buffers are being prepared;
           otherwise a short burst of half-initialised load audio leaks out. */
        ndspChnSetPaused(AUDIO_CHANNEL, true);
    }
    apply_output_rate();
    loaded = true;
    const bool first = fill_wavebuf(0);
    const bool second = fill_wavebuf(1);
    if (!first && !second) { audio_stop(); status = "MP3 DECODE ERROR"; return false; }
    if (ndsp_available) ndspChnSetPaused(AUDIO_CHANNEL, true);
    snprintf(current_path, sizeof(current_path), "%s", path);
    status = "MP3 LOADED  B:PLAY";
    return true;
}

bool audio_load_m4a(const char *path) {
    if (!initialized) { status = "NDSP NOT READY"; return false; }
    if (!path || !path[0]) { status = "M4A PATH ERROR"; return false; }
    audio_stop();
    if (!preserving_hot_cue_cache) memset(hot_cue_cached, 0, sizeof(hot_cue_cached));
    if (mp4read_open((char *)path) != 0) { status = "M4A OPEN ERROR"; return false; }
    aac_decoder = NeAACDecOpen();
    if (!aac_decoder) { mp4read_close(); status = "AAC INIT ERROR"; return false; }
    NeAACDecConfigurationPtr config = NeAACDecGetCurrentConfiguration(aac_decoder);
    if (!config) { audio_stop(); status = "AAC CONFIG ERROR"; return false; }
    config->outputFormat = FAAD_FMT_16BIT;
    config->downMatrix = 1;
    if (!NeAACDecSetConfiguration(aac_decoder, config)) { audio_stop(); status = "AAC CONFIG ERROR"; return false; }

    unsigned long rate = 0;
    unsigned char channels = 0;
    if (NeAACDecInit2(aac_decoder, mp4config.asc.buf, mp4config.asc.size,
                      &rate, &channels) != 0 || rate == 0 || channels == 0) {
        audio_stop();
        status = "AAC STREAM ERROR";
        return false;
    }
    sample_rate = (u32)rate;
    if (master_tempo_enabled) stretch_reset(sample_rate, 1.0f + tempo_percent / 100.0f);
    source_finished = false;
    is_aac = true;
    aac_pending_count = aac_pending_offset = 0;
    aac_initial_skip_samples = gapless_delay_samples;
    aac_decode_cursor_ms = 0;
    aac_source_position_samples = 0;
    aac_decode_error_streak = 0;
    reset_transport(0);
    mt_output_source_samples = 0.0;
    if (ndsp_available) {
        ndspChnReset(AUDIO_CHANNEL);
        ndspChnSetInterp(AUDIO_CHANNEL, NDSP_INTERP_LINEAR);
        ndspChnSetFormat(AUDIO_CHANNEL, NDSP_FORMAT_STEREO_PCM16);
        ndspChnSetPaused(AUDIO_CHANNEL, true);
    }
    apply_output_rate();
    loaded = true;
    const bool first = fill_wavebuf(0);
    const bool second = fill_wavebuf(1);
    if (!first && !second) { audio_stop(); status = "AAC DECODE ERROR"; return false; }
    if (ndsp_available) ndspChnSetPaused(AUDIO_CHANNEL, true);
    snprintf(current_path, sizeof(current_path), "%s", path);
    status = "M4A LOADED  B:PLAY";
    return true;
}

void audio_set_paused(bool paused) {
    if (loaded) {
        if (ndsp_available) ndspChnSetPaused(AUDIO_CHANNEL, paused);
        transport_running = !paused;
        transport_anchor_tick = osGetTime();
        status = paused ? "AUDIO PAUSED" : "AUDIO PLAYING";
    }
}

void audio_set_tempo(float percent) {
    const bool was_processing_mt = needs_master_tempo_processing();
    tempo_percent = percent;
    const bool enters_mt_processing = !was_processing_mt && needs_master_tempo_processing();
    /* At +/-0 the ordinary decoder feeds the two queued NDSP buffers directly.
       When MT starts, SoundTouch's output timeline must begin after those
       buffers, not at the position left by the last seek (often track start).
       Otherwise the first +tempo buffer makes only the waveform jump back. */
    if (enters_mt_processing)
        mt_output_source_samples = (double)(is_aac ? aac_source_position_samples : source_position_samples);
    if (needs_master_tempo_processing())
        loop_transition_requested = false;
    if (master_tempo_enabled) stretch_set_tempo(1.0f + percent / 100.0f);
    if (initialized) apply_output_rate();
}

void audio_set_pitch_bend(float percent) {
    if (percent > 6.0f) percent = 6.0f;
    if (percent < -6.0f) percent = -6.0f;
    if (fabsf(percent - pitch_bend_percent) < 0.02f) return;
    pitch_bend_percent = percent;
    if (initialized) apply_output_rate();
}

bool audio_set_master_tempo(bool enabled) {
    const bool was_processing_mt = needs_master_tempo_processing();
    master_tempo_enabled = enabled;
    if (!was_processing_mt && needs_master_tempo_processing())
        mt_output_source_samples = (double)(is_aac ? aac_source_position_samples : source_position_samples);
    if (needs_master_tempo_processing())
        loop_transition_requested = false;
    if (master_tempo_enabled) stretch_reset(sample_rate, 1.0f + tempo_percent / 100.0f);
    apply_output_rate();
    return true;
}

void audio_set_duration_ms(uint32_t milliseconds) {
    stream_duration_ms = milliseconds;
}

void audio_set_gapless_delay_samples(uint32_t samples) {
    gapless_delay_samples = samples;
}

void audio_set_mp3_sample_rate(uint32_t samples_per_second) {
    if (samples_per_second >= 8000 && samples_per_second <= 48000)
        mp3_source_sample_rate = samples_per_second;
    else
        mp3_source_sample_rate = 44100;
}

void audio_set_mp3_seek_index(const uint32_t *offsets, const uint16_t *skips, uint32_t count) {
    mp3_seek_count = count > MP3_SEEK_POINTS ? MP3_SEEK_POINTS : count;
    if (offsets && mp3_seek_count) {
        bool any_offset = false;
        memcpy(mp3_seek_offsets, offsets, mp3_seek_count * sizeof(u32));
        if (skips) memcpy(mp3_seek_skips, skips, mp3_seek_count * sizeof(u16));
        for (u32 i = 0; i < mp3_seek_count; ++i) if (mp3_seek_offsets[i]) { any_offset = true; break; }
        if (!any_offset) mp3_seek_count = 0;
    } else mp3_seek_count = 0;
}

static bool audio_seek_internal(u32 milliseconds, bool fast, bool reverse, bool scrub, float speed) {
    if (!initialized || !loaded || !current_path[0]) return false;
    /* A scrub is a succession of short, independently seekable PCM grains.
       Apply its rate before the channel reset so the first audible sample has
       the drag velocity rather than the regular deck tempo. */
    scrub_preview = scrub;
    scrub_speed = speed < 0.125f ? 0.125f : speed > 8.0f ? 8.0f : speed;
    if (is_aac) {
        /* Do not route an AAC performance seek through audio_load_m4a(): that
           path decodes two start-of-file buffers, tears down NDSP, then seeks
           again. Reopening once at the exact AAC access unit keeps the Hot Cue
           cache valid and prevents a failed initial decode poisoning the deck. */
        if (!prepare_m4a_source(milliseconds, true, fast, false)) return false;
        scrub_preview = scrub;
        scrub_speed = speed < 0.125f ? 0.125f : speed > 8.0f ? 8.0f : speed;
    } else {
        /* The cache provides a fixed decoder start and exact sample discard
           count for every position. There is deliberately no runtime scan or
           byte-ratio seek: the same cue always yields the same source sample. */
        char path[sizeof(current_path)]; snprintf(path, sizeof(path), "%s", current_path);
        /* Do not call audio_load_mp3 here: that routine would decode and queue
           two buffers from the start of the song, discard them, reset again,
           and only then decode the cue.  Reopen directly at the deterministic
           seek anchor so Hot Cue and Beat Jump spend their time on audio. */
        if (!prepare_mp3_source(milliseconds, true, fast, false)) return false;
    }
    reverse_first_buffer = reverse;
    const bool first = fill_wavebuf(0);
    reverse_first_buffer = false;
    /* Fast performance seeks start as soon as the target's first complete
       chunk is ready. audio_update immediately supplies the second free
       buffer, without holding back audible Hot Cue/Beat Jump playback. */
    const bool second = fast ? true : fill_wavebuf(1);
    if (!first && !second) { status = "SEEK DECODE ERROR"; return false; }
    if (ndsp_available) ndspChnSetPaused(AUDIO_CHANNEL, true);
    status = "CUE READY";
    return true;
}

static bool prepare_mp3_source(u32 milliseconds, bool reset_channel, bool fast_reset,
                               bool preserve_stretch) {
    char path[sizeof(current_path)];
    snprintf(path, sizeof(path), "%s", current_path);
    if (reset_channel) reset_channel_for_seek(fast_reset);
    if (stream) { fclose(stream); stream = NULL; }
    stream = fopen(path, "rb");
    if (!stream) { loaded = false; status = "MP3 OPEN ERROR"; return false; }
    mp3dec_init(&decoder);
    input_size = input_offset = 0;
    source_finished = false;
    is_aac = false;
    sample_rate = mp3_source_sample_rate;
    if (master_tempo_enabled && !preserve_stretch)
        stretch_reset(sample_rate, 1.0f + tempo_percent / 100.0f);
    apply_output_rate();
    loaded = true;
    mp3_initial_skip_samples = 0;
    if (milliseconds == 0) {
        mp3_initial_skip_samples = gapless_delay_samples;
    } else if (mp3_seek_count > 1) {
        const u32 raw_target_samples = gapless_delay_samples +
            (u32)((u64)milliseconds * sample_rate / 1000u);
        const u32 raw_target_ms = (u32)((u64)raw_target_samples * 1000u / sample_rate);
        u32 index = raw_target_ms / MP3_SEEK_STEP_MS;
        if (index >= mp3_seek_count) index = mp3_seek_count - 1;
        const u32 anchor_ms = index * MP3_SEEK_STEP_MS;
        if (fseek(stream, (long)mp3_seek_offsets[index], SEEK_SET)) return false;
        const u32 anchor_samples = (u32)((u64)anchor_ms * sample_rate / 1000u);
        mp3_initial_skip_samples = mp3_seek_skips[index] +
            (raw_target_samples > anchor_samples ? raw_target_samples - anchor_samples : 0);
    } else {
        status = "MP3 SEEK INDEX MISSING";
        return false;
    }
    source_position_samples = (u64)milliseconds * sample_rate / 1000u;
    if (!preserve_stretch) {
        reset_transport(source_position_samples);
        mt_output_source_samples = (double)source_position_samples;
    }
    return true;
}

/* Reopen and pre-roll AAC while a Hot Cue is being played from PCM memory.
   Unlike audio_load_m4a/audio_seek_internal this intentionally never resets
   NDSP or queues a wave buffer: the cached cue must remain audible until the
   prepared decoder can take over at its exact source-time boundary. */
static bool prepare_m4a_source(u32 milliseconds, bool reset_channel, bool fast_reset,
                               bool preserve_stretch) {
    if (!current_path[0]) return false;
    if (aac_decoder) { NeAACDecClose(aac_decoder); aac_decoder = NULL; }
    mp4read_close();
    if (mp4read_open(current_path) != 0) { status = "M4A OPEN ERROR"; return false; }
    aac_decoder = NeAACDecOpen();
    if (!aac_decoder) { mp4read_close(); status = "AAC INIT ERROR"; return false; }
    NeAACDecConfigurationPtr config = NeAACDecGetCurrentConfiguration(aac_decoder);
    if (!config) { NeAACDecClose(aac_decoder); aac_decoder = NULL; mp4read_close(); return false; }
    config->outputFormat = FAAD_FMT_16BIT;
    config->downMatrix = 1;
    if (!NeAACDecSetConfiguration(aac_decoder, config)) return false;
    unsigned long rate = 0;
    unsigned char channels = 0;
    if (NeAACDecInit2(aac_decoder, mp4config.asc.buf, mp4config.asc.size,
                      &rate, &channels) != 0 || !rate || !channels) return false;
    sample_rate = (u32)rate;
    if (master_tempo_enabled && !preserve_stretch)
        stretch_reset(sample_rate, 1.0f + tempo_percent / 100.0f);
    source_finished = false;
    is_aac = true;
    aac_pending_count = aac_pending_offset = 0;
    aac_decode_cursor_ms = 0;
    aac_initial_skip_samples = 0;
    aac_source_position_samples = 0;
    aac_decode_error_streak = 0;
    if (reset_channel) reset_channel_for_seek(fast_reset);
    apply_output_rate();
    loaded = true;
    if (milliseconds == 0) {
        aac_initial_skip_samples = gapless_delay_samples;
        if (!preserve_stretch) {
            reset_transport(0);
            mt_output_source_samples = 0.0;
        }
        return true;
    }
    const u32 target_sample = gapless_delay_samples +
        (u32)((u64)milliseconds * sample_rate / 1000u);
    const u32 target_frame = target_sample / 1024u;
    const u32 preroll = target_frame > 12 ? target_frame - 12 : 0;
    if (preroll >= mp4config.frame.nsamples) return false;
    mp4config.frame.current = preroll;
    aac_decode_cursor_ms = (u32)((u64)preroll * 1024u * 1000u / sample_rate);
    if (!prime_aac_to_frame(target_frame)) return false;
    aac_pending_count = aac_pending_offset = 0;
    aac_initial_skip_samples = target_sample - target_frame * 1024u;
    aac_source_position_samples = (u64)milliseconds * sample_rate / 1000u;
    if (!preserve_stretch) {
        reset_transport(aac_source_position_samples);
        mt_output_source_samples = (double)aac_source_position_samples;
    }
    return true;
}

bool audio_prepare_loop_cache(u32 start_ms, u32 end_ms, u32 restore_ms) {
    if (!loaded || !loop_cache || end_ms <= start_ms) return false;
    const u64 loop_samples = (u64)(end_ms - start_ms) * sample_rate / 1000u;
    if (!loop_samples) return false;
    (void)restore_ms;
    /* Do not decode the loop synchronously in the touch handler.  Seek only
       the first ready buffer, then capture this first live pass in queueing
       order while NDSP plays it. */
    const int wanted = (int)(((u64)LOOP_CACHE_MS * sample_rate + 999u) / 1000u);
    loop_start_ms = start_ms;
    loop_end_ms = end_ms;
    loop_start_samples = (u64)start_ms * sample_rate / 1000u;
    loop_end_samples = (u64)end_ms * sample_rate / 1000u;
    loop_capture_target_frames = loop_samples < (u64)wanted ? (int)loop_samples : wanted;
    loop_capture_output_frames = wanted < LOOP_CACHE_FRAMES ? wanted : LOOP_CACHE_FRAMES;
    loop_capture_frames = 0;
    loop_capture_next_samples = loop_start_samples;
    loop_cache_start_samples = loop_start_samples;
    loop_capture_active = true;
    loop_cache_ready = false;
    const bool saved_loop = loop_enabled;
    loop_enabled = false;
    const bool seeked = audio_seek_internal(start_ms, true, false, false, 1.0f);
    loop_enabled = saved_loop;
    if (!seeked) loop_capture_active = false;
    return seeked;
}

void audio_prime_loop_in(u32 start_ms) {
    if (!loaded || !loop_cache) return;
    const int wanted = (int)(((u64)LOOP_CACHE_MS * sample_rate + 999u) / 1000u);
    loop_enabled = false;
    loop_start_ms = start_ms;
    loop_end_ms = start_ms + LOOP_CACHE_MS;
    loop_start_samples = (u64)start_ms * sample_rate / 1000u;
    loop_end_samples = loop_start_samples + (u64)wanted;
    loop_capture_target_frames = wanted < LOOP_CACHE_FRAMES ? wanted : LOOP_CACHE_FRAMES;
    loop_capture_output_frames = loop_capture_target_frames;
    loop_capture_frames = 0;
    loop_capture_next_samples = loop_start_samples;
    loop_cache_start_samples = loop_start_samples;
    loop_capture_active = true;
    loop_cache_ready = false;
    /* The rolling source cache usually already covers the selected beat, so
       Loop In itself needs no seek or decoder work. */
    if (live_scratch_valid)
        append_loop_capture(scratch_cache, live_scratch_frames, live_scratch_start_samples);
}

bool audio_arm_loop_capture(u32 start_ms, u32 end_ms) {
    if (!loaded || !loop_cache || end_ms <= start_ms) return false;
    const u64 requested_start = (u64)start_ms * sample_rate / 1000u;
    /* Never identify a previous Cue's PCM as the new loop just because the
       requested duration happens to fit in it. */
    if (loop_cache_start_samples != requested_start) {
        loop_cache_ready = false;
        loop_capture_frames = 0;
        loop_cache_frames = 0;
        loop_cache_start_samples = requested_start;
    }
    audio_resize_loop(start_ms, end_ms);
    if (loop_cache_ready) return true;
    const u64 loop_samples = loop_end_samples - loop_start_samples;
    const int wanted = (int)(((u64)LOOP_CACHE_MS * sample_rate + 999u) / 1000u);
    loop_capture_target_frames = loop_samples < (u64)wanted ? (int)loop_samples : wanted;
    loop_capture_output_frames = wanted < LOOP_CACHE_FRAMES ? wanted : LOOP_CACHE_FRAMES;
    loop_capture_frames = 0;
    loop_capture_next_samples = loop_start_samples;
    loop_capture_active = true;
    loop_cache_ready = false;
    if (live_scratch_valid)
        append_loop_capture(scratch_cache, live_scratch_frames, live_scratch_start_samples);
    return loop_capture_frames > 0 || loop_cache_ready;
}

static void begin_loop_cache_transition(void) {
    loop_transition_requested = false;
    if (!can_use_direct_loop_cache() || loop_cache_frames <= 0) return;
    loop_cache_playing = true;
    loop_source_queued = false;
    loop_source_prepared = false;
    loop_transport_restarted = false;
    loop_clock_active = false;
    loop_buffer_start_samples = loop_start_samples;
    loop_buffer_end_samples = loop_end_samples;
    loop_buffer_resume_samples = loop_resume_samples;
    loop_resume_pending = true;
    loop_resume_prepare_after = osGetTime() + 20;
    if (ndsp_available) {
        const size_t bytes = (size_t)loop_cache_frames * 2 * sizeof(s16);
        DSP_FlushDataCache(loop_cache, bytes);
        memset(&loop_wavebuf, 0, sizeof(loop_wavebuf));
        loop_wavebuf.data_vaddr = loop_cache;
        loop_wavebuf.nsamples = (u32)loop_cache_frames;
        ndspChnWaveBufAdd(AUDIO_CHANNEL, &loop_wavebuf);
    }
}

/* MT cannot submit loop_cache directly because its PCM is native-rate. Feed
   that already-decoded Loop-In segment through the live SoundTouch instance
   instead, then seek the decoder while SoundTouch has roughly one second of
   output to supply. This moves file reopen/AAC preroll away from Loop Out and
   keeps the overlap state that determines both pitch and seamlessness. */
static bool begin_master_loop_cache_transition(void) {
    if (!can_use_master_loop_cache()) return false;
    stretch_put(loop_cache, (u32)loop_cache_frames);
    const bool prepared = is_aac ?
        prepare_m4a_source(loop_resume_ms, false, true, true) :
        prepare_mp3_source(loop_resume_ms, false, true, true);
    if (!prepared) return false;
    if (is_aac)
        align_aac_source_samples(loop_resume_samples, loop_resume_ms);
    else
        align_mp3_source_samples(loop_resume_samples, loop_resume_ms);
    return true;
}

void audio_set_loop(u32 start_ms, u32 end_ms, bool enabled) {
    /* AAC/M4A follows the same pre-expanded PCM boundary path as MP3. */
    const u64 exit_position = !enabled ? current_transport_samples() : 0;
    loop_enabled = enabled && end_ms > start_ms;
    loop_start_ms = start_ms;
    loop_end_ms = end_ms;
    loop_start_samples = (u64)start_ms * sample_rate / 1000u;
    loop_end_samples = (u64)end_ms * sample_rate / 1000u;
    if (!loop_enabled) {
        loop_capture_active = false;
        loop_cache_ready = false;
        loop_transition_requested = false;
        /* A direct Loop PCM buffer can already be playing. Do not abandon the
           state machine here: it owns the queued continuation. Just stop its
           cyclic display clock and let the final buffer drain cleanly. */
        if (loop_cache_playing) {
            loop_clock_active = false;
            reset_transport(exit_position);
        } else {
            loop_resume_pending = false;
        loop_source_queued = false;
        loop_source_prepared = false;
            loop_transport_restarted = false;
        }
    }
}

void audio_resize_loop(u32 start_ms, u32 end_ms) {
    if (end_ms <= start_ms) return;
    loop_enabled = true;
    loop_start_ms = start_ms;
    loop_end_ms = end_ms;
    loop_start_samples = (u64)start_ms * sample_rate / 1000u;
    loop_end_samples = (u64)end_ms * sample_rate / 1000u;
    const u64 loop_samples = loop_end_samples - loop_start_samples;
    const int wanted = (int)(((u64)LOOP_CACHE_MS * sample_rate + 999u) / 1000u);
    const int target = loop_samples < (u64)wanted ? (int)loop_samples : wanted;
    const int output = wanted < LOOP_CACHE_FRAMES ? wanted : LOOP_CACHE_FRAMES;
    /* NDSP reads loop_cache directly.  Never rewrite it while its previous
       version is queued or audible: that produced the random-location audio
       heard after x2/x1/2.  The next source pass builds the new cache. */
    if (loop_cache_playing || loop_wavebuf.status == NDSP_WBUF_PLAYING ||
        loop_wavebuf.status == NDSP_WBUF_QUEUED) {
        loop_cache_ready = false;
        loop_capture_active = false;
        return;
    }
    /* A shorter loop is wholly contained in the raw Loop-In segment already
       captured for the old loop. Repeating that prefix is memory-only and
       never seeks, decodes, or moves transport. */
    if (loop_cache_start_samples == loop_start_samples &&
        loop_capture_frames >= target && target > 0 && loop_cache) {
        for (int frame = target; frame < output; ++frame) {
            const int source = frame % target;
            loop_cache[frame * 2] = loop_cache[source * 2];
            loop_cache[frame * 2 + 1] = loop_cache[source * 2 + 1];
        }
        loop_capture_target_frames = target;
        loop_capture_output_frames = output;
        loop_cache_frames = output;
        loop_cache_duration_ms = (u32)((u64)output * 1000u / sample_rate);
        loop_resume_samples = loop_start_samples + ((u64)output % loop_samples);
        loop_resume_ms = (u32)(loop_resume_samples * 1000u / sample_rate);
        loop_cache_ready = true;
    } else {
        /* A longer first segment has not yet been decoded.  Keep playback at
           its current source location; the next pass will refill normally
           rather than stalling this x2 pad press. */
        loop_cache_ready = false;
        loop_capture_active = false;
    }
}

bool audio_has_native_loop(void) { return loaded && loop_enabled && can_use_direct_loop_cache(); }

static bool cache_hot_cue(u8 number, u32 time_ms) {
    if (!loaded || !hot_cue_cache || !number || number > PERFORMANCE_CACHE_SLOTS) return false;
    int slot = -1;
    for (int i = 0; i < PERFORMANCE_CACHE_SLOTS; ++i) {
        if (hot_cue_cached[i] && hot_cue_numbers[i] == number) { slot = i; break; }
        if (slot < 0 && !hot_cue_cached[i]) slot = i;
    }
    if (slot < 0) return false;

        scratch_capture = true;
        captured_frames[0] = captured_frames[1] = 0;
        if (!audio_seek_internal(time_ms, false, false, false, 1.0f)) {
            scratch_capture = false;
            return false;
        }
        int needed = (int)(((u64)HOT_CUE_CACHE_MS * sample_rate + 999u) / 1000u);
        if (needed < 1) needed = 1;
        if (needed > HOT_CUE_CACHE_FRAMES) needed = HOT_CUE_CACHE_FRAMES;
        s16 *destination = hot_cue_cache + slot * HOT_CUE_CACHE_FRAMES * 2;
        int written = 0, index = 0;
        while (written < needed) {
            const int available = captured_frames[index];
            if (available <= 0) break;
            const int take = available < needed - written ? available : needed - written;
            memcpy(destination + written * 2, pcm[index], (size_t)take * 2 * sizeof(s16));
            written += take;
            if (written >= needed) break;
            captured_frames[index] = 0;
            if (!fill_wavebuf(index)) break;
            index ^= 1;
        }
        scratch_capture = false;
        if (written <= 0) return false;
        hot_cue_numbers[slot] = number;
        hot_cue_times[slot] = time_ms;
        hot_cue_cache_frames[slot] = written;
        hot_cue_duration_ms[slot] = (u32)((u64)written * 1000u / sample_rate);
        hot_cue_start_samples[slot] = (u64)time_ms * sample_rate / 1000u;
        hot_cue_resume_samples[slot] = hot_cue_start_samples[slot] + (u64)written;
        hot_cue_cached[slot] = true;
        return true;
}

void audio_preload_hot_cues(const u32 *times, const u8 *numbers, u32 count,
                            u32 bpm_100, u32 restore_ms) {
    (void)bpm_100;
    memset(hot_cue_cached, 0, sizeof(hot_cue_cached));
    if (!loaded || !times || !numbers || !hot_cue_cache) return;
    preserving_hot_cue_cache = true;
    for (u32 i = 0; i < count; ++i) {
        if (!numbers[i] || numbers[i] > HOT_CUE_SLOTS || times[i] == 0xffffffffu) continue;
        cache_hot_cue(numbers[i], times[i]);
    }
    audio_seek_ms(restore_ms);
    preserving_hot_cue_cache = false;
}

bool audio_cache_hot_cue(u8 number, u32 milliseconds, u32 restore_ms) {
    if (!loaded) return false;
    preserving_hot_cue_cache = true;
    const bool cached = cache_hot_cue(number, milliseconds);
    /* Caching is a one-off operation when a pad is created.  Restore the
       original deck source before handing control back to the performance
       path; a later press is always cache-only. */
    const bool restored = audio_seek_ms(restore_ms);
    preserving_hot_cue_cache = false;
    return cached && restored;
}

bool audio_cached_hot_cue_matches(u8 number, u32 milliseconds) {
    for (int slot = 0; slot < PERFORMANCE_CACHE_SLOTS; ++slot)
        if (hot_cue_cached[slot] && hot_cue_numbers[slot] == number && hot_cue_times[slot] == milliseconds)
            return true;
    return false;
}

bool audio_trigger_preloaded_hot_cue(u8 number) {
    if (!hot_cue_cache) return false;
    for (int slot = 0; slot < PERFORMANCE_CACHE_SLOTS; ++slot) {
        if (!hot_cue_cached[slot] || hot_cue_numbers[slot] != number) continue;
        reset_channel_for_seek(true);
        const s16 *source = hot_cue_cache + slot * HOT_CUE_CACHE_FRAMES * 2;
        hotcue_cache_source = source;
        hotcue_cache_total_frames = hot_cue_cache_frames[slot];
        hotcue_cache_next_frame = 0;
        hotcue_cache_playing = true;
        hotcue_source_queued = false;
        if (ndsp_available) {
            const size_t bytes = (size_t)hotcue_cache_total_frames * 2 * sizeof(s16);
            DSP_FlushDataCache((void *)source, bytes);
            memset(&hotcue_wavebuf, 0, sizeof(hotcue_wavebuf));
            hotcue_wavebuf.data_vaddr = (void *)source;
            hotcue_wavebuf.nsamples = (u32)hotcue_cache_total_frames;
            ndspChnWaveBufAdd(AUDIO_CHANNEL, &hotcue_wavebuf);
        }
        if (ndsp_available) ndspChnSetPaused(AUDIO_CHANNEL, true);
        /* The pad path is now only memcpy + NDSP queue.  Give that first
           buffer a tiny head start before touching the SD card again: doing
           the reopen in the same UI tick made slow cards affect pad latency. */
        hotcue_start_samples = hot_cue_start_samples[slot];
        hotcue_resume_samples = hot_cue_resume_samples[slot];
        hotcue_resume_ms = (u32)((hotcue_resume_samples * 1000u) / sample_rate);
        /* This clock is deliberately independent of decoder preparation. */
        hotcue_clock_tick = osGetTime();
        hotcue_clock_active = true;
        reset_transport(hotcue_start_samples);
        hotcue_resume_pending = true;
        hotcue_resume_fallback = false;
        /* Let the direct PCM cache reach NDSP and give the UI several frames
           to advance its waveform marker before the decoder performs its
           (potentially expensive) continuation seek.  The cache is one full
           second, so this does not put the audible hand-off at risk. */
        hotcue_resume_prepare_after = osGetTime() + 160;
        status = "CUE READY";
        return true;
    }
    return false;
}

bool audio_seek_ms(u32 milliseconds) {
    return audio_seek_internal(milliseconds, false, false, false, 1.0f);
}

bool audio_seek_hot_cue_ms(u32 milliseconds) {
    return audio_seek_internal(milliseconds, true, false, false, 1.0f);
}

bool audio_scrub_ms(u32 milliseconds, bool reverse, float speed) {
    return audio_seek_internal(milliseconds, true, reverse, true, speed);
}

bool audio_begin_scrub(u32 milliseconds) {
    const u64 target = (u64)milliseconds * sample_rate / 1000u;
    if ((!live_scratch_valid || target < live_scratch_start_samples ||
         target >= live_scratch_start_samples + (u64)live_scratch_frames) &&
        !snapshot_queued_scratch(target))
        return false;
    reset_channel_for_seek(true);
    scratch_active = true;
    scratch_cache_start_ms = (u32)(live_scratch_start_samples * 1000u / sample_rate);
    scratch_cache_frames = live_scratch_frames;
    scratch_position_frames = (float)(target - live_scratch_start_samples);
    scratch_rate_frames = 0.0f;
    /* Do not queue a repeated static sample.  A platter at rest is silent. */
    scratch_stationary = true;
    return true;
}

bool audio_set_scrub_position(u32 milliseconds, bool reverse, float speed) {
    if (!scratch_active) return false;
    float position = (float)((int64_t)milliseconds - (int64_t)scratch_cache_start_ms) * sample_rate / 1000.0f;
    if (position < 32.0f || position > (float)scratch_cache_frames - 32.0f) {
        /* Never launch a fresh AAC/MP3 seek from a touch event: it stalls
           touch scanning and makes both waveform and audio appear frozen.
           The visual transport follows the finger; release commits one exact
           seek. Motion inside this prepared PCM window remains audible. */
        audio_hold_scrub();
        return false;
    }
    scratch_position_frames = position;
    if (speed < 0.18f) { audio_hold_scrub(); return false; }
    /* The rate is the measured touch travel/time itself, so displayed
       waveform motion and audible source motion stay in the same basis. */
    float rate = speed;
    if (rate > 4.0f) rate = 4.0f;
    scratch_rate_frames = reverse ? -rate : rate;
    scratch_stationary = false;
    /* Drop the previous 50 ms grain rather than letting it play after the
       finger changed velocity.  Two 512-frame grains provide 20+ ms of
       audio headroom while keeping jog response immediate. */
    if (ndsp_available) ndspChnWaveBufClear(AUDIO_CHANNEL);
    queue_scratch_wavebuf(0);
    queue_scratch_wavebuf(1);
    return true;
}

void audio_hold_scrub(void) {
    if (!scratch_active || scratch_stationary) return;
    scratch_stationary = true;
    scratch_rate_frames = 0.0f;
    if (ndsp_available) {
        ndspChnWaveBufClear(AUDIO_CHANNEL);
        ndspChnSetPaused(AUDIO_CHANNEL, true);
    }
}

void audio_end_scrub(void) {
    scratch_active = false;
    scratch_rate_frames = 0.0f;
    scratch_stationary = true;
}

void audio_update(void) {
    if (!loaded || !ndsp_available) return;
    if (loop_cache_playing) {
        /* The direct loop buffer follows the final pre-boundary stream buffer.
           Reset the visible source clock only when NDSP has actually started
           that direct buffer, not when it was merely queued. */
        if (!loop_transport_restarted && loop_wavebuf.status == NDSP_WBUF_PLAYING) {
            reset_transport(loop_buffer_start_samples);
            loop_clock_tick = osGetTime();
            loop_clock_active = true;
            loop_transport_restarted = true;
        }
        if (loop_resume_pending && loop_wavebuf.status == NDSP_WBUF_PLAYING &&
            osGetTime() >= loop_resume_prepare_after) {
            loop_resume_pending = false;
            const bool prepared = is_aac ? prepare_m4a_source(loop_resume_ms, false, false, false)
                                         : prepare_mp3_source(loop_resume_ms, false, false, false);
            if (prepared) {
                if (is_aac)
                    align_aac_source_samples(loop_resume_samples, loop_resume_ms);
                else
                    align_mp3_source_samples(loop_resume_samples, loop_resume_ms);
                /* Do not overwrite a regular PCM buffer while NDSP is still
                   consuming it.  That race was the remaining click/drop at
                   Loop Out.  The direct two-second loop PCM gives us time to
                   queue these only after their old ownership is finished. */
                loop_source_prepared = true;
            }
        }
        if (loop_source_prepared) {
            for (int i = 0; i < 2; ++i) {
                if (wavebuf[i].status != NDSP_WBUF_DONE && wavebuf[i].status != NDSP_WBUF_FREE)
                    continue;
                if (fill_wavebuf(i)) loop_source_queued = true;
            }
        }
        if (loop_wavebuf.status == NDSP_WBUF_DONE || loop_wavebuf.status == NDSP_WBUF_FREE) {
            loop_cache_playing = false;
            loop_clock_active = false;
            /* The following stream buffers were prepared at exactly this
               source point.  Anchor the wave on their first sample instead
               of letting it retain the preceding direct-buffer clock. */
            if (loop_enabled)
                reset_transport(loop_buffer_resume_samples);
        }
        return;
    }
    if (hotcue_resume_pending && osGetTime() >= hotcue_resume_prepare_after) {
        /* The decoder must seek ahead to the end of the cached cue, but that
           source position is not the audible/playhead position yet. Preserve
           the latter across the background prepare to avoid a visual jump. */
        const u64 audible_samples = current_transport_samples();
        hotcue_resume_pending = false;
        const bool prepared = is_aac ? prepare_m4a_source(hotcue_resume_ms, false, false, false)
                                     : prepare_mp3_source(hotcue_resume_ms, false, false, false);
        if (!prepared) {
            /* Leave the already queued PCM cue audible.  At its boundary we
               retry the conventional seek instead of leaving AAC/M4A silent. */
            hotcue_resume_fallback = true;
            return;
        }
        if (is_aac)
            align_aac_source_samples(hotcue_resume_samples, hotcue_resume_ms);
        else
            align_mp3_source_samples(hotcue_resume_samples, hotcue_resume_ms);
        /* Keep the direct PCM clock authoritative until its final sample is
           consumed.  prepare_* resets decoder state, never presentation. */
        (void)audible_samples;
        /* This is queued after the direct PCM cue buffer, not played now.
           It gives the decoder a full cache second to seek and eliminates
           the per-track stall at the pad's first sample. */
        const bool first_source_buffer = fill_wavebuf(0);
        const bool second_source_buffer = fill_wavebuf(1);
        if (!first_source_buffer && !second_source_buffer) {
            loaded = false;
            return;
        }
        hotcue_source_queued = true;
    }
    if (hotcue_cache_playing) {
        if (hotcue_wavebuf.status == NDSP_WBUF_DONE || hotcue_wavebuf.status == NDSP_WBUF_FREE) {
            /* The next queued stream buffer begins at this exact source
               sample.  Anchor once at the hand-off; no ms rounding. */
            hotcue_clock_active = false;
            reset_transport(hotcue_resume_samples);
            hotcue_cache_playing = false;
            if (!hotcue_source_queued && hotcue_resume_fallback) {
                hotcue_resume_fallback = false;
                if (audio_seek_hot_cue_ms(hotcue_resume_ms)) {
                    audio_set_paused(false);
                }
            }
        }
        return;
    }
    if (scratch_active) {
        for (int i = 0; i < 2; ++i)
            if (wavebuf[i].status == NDSP_WBUF_DONE || wavebuf[i].status == NDSP_WBUF_FREE)
                queue_scratch_wavebuf(i);
        return;
    }
    if (scrub_preview) return;
    for (int i = 0; i < 2; ++i)
        if (wavebuf[i].status == NDSP_WBUF_DONE || wavebuf[i].status == NDSP_WBUF_FREE) {
            if (wavebuf[i].status == NDSP_WBUF_DONE) mark_completed_wavebuf(i);
            if (source_finished) {
                /* Natural end-of-track is not an unload.  Keeping decoder
                   state valid lets a waveform rewind perform one ordinary
                   seek and B can play again without reloading the track. */
                transport_running = false;
                if (stream_duration_ms)
                    reset_transport((u64)stream_duration_ms * sample_rate / 1000u);
                status = "END OF TRACK";
                continue;
            }
            if (!fill_wavebuf(i) && source_finished) {
                transport_running = false;
                if (stream_duration_ms)
                    reset_transport((u64)stream_duration_ms * sample_rate / 1000u);
                status = "END OF TRACK";
            }
            if (loop_cache_playing) break;
        }
}

void audio_stop(void) {
    if (!initialized) return;
    if (ndsp_available) {
        ndspChnSetPaused(AUDIO_CHANNEL, true);
        ndspChnReset(AUDIO_CHANNEL);
    }
    /* Do not reuse the two stream buffers until the DSP has acknowledged reset.
       This prevents an X-load during playback from touching a queued buffer. */
    if (ndsp_available) svcSleepThread(20 * 1000 * 1000LL);
    if (stream) { fclose(stream); stream = NULL; }
    if (aac_decoder) { NeAACDecClose(aac_decoder); aac_decoder = NULL; }
    mp4read_close();
    input_size = input_offset = 0;
    mp3_initial_skip_samples = 0;
    aac_initial_skip_samples = 0;
    aac_pending_count = aac_pending_offset = 0;
    aac_decode_cursor_ms = 0;
    aac_source_position_samples = 0;
    aac_decode_error_streak = 0;
    source_finished = false;
    is_aac = false;
    scrub_preview = false;
    reverse_first_buffer = false;
    scratch_active = false;
    scratch_cache_frames = 0;
    live_scratch_valid = false;
    live_scratch_frames = 0;
    live_scratch_start_samples = 0;
    pitch_bend_percent = 0.0f;
    loop_enabled = false;
    loop_cache_ready = false;
    loop_capture_active = false;
    loop_capture_frames = 0;
    loop_cache_start_samples = (u64)-1;
    loop_transition_requested = false;
    loop_cache_playing = false;
    loop_clock_active = false;
    loop_resume_pending = false;
    loop_source_queued = false;
    loop_source_prepared = false;
    loop_transport_restarted = false;
    hotcue_resume_pending = false;
    hotcue_resume_fallback = false;
    hotcue_resume_prepare_after = 0;
    hotcue_cache_playing = false;
    hotcue_clock_active = false;
    hotcue_source_queued = false;
    memset(&hotcue_wavebuf, 0, sizeof(hotcue_wavebuf));
    wavebuf_source_known[0] = wavebuf_source_known[1] = false;
    transport_running = false;
    reset_transport(0);
    mt_output_source_samples = 0.0;
    loaded = false;
}

void audio_exit(void) {
    if (!initialized) return;
    audio_stop();
    if (pcm[0]) linearFree(pcm[0]);
    if (pcm[1]) linearFree(pcm[1]);
    if (scratch_cache) linearFree(scratch_cache);
    if (hot_cue_cache) linearFree(hot_cue_cache);
    if (loop_cache) linearFree(loop_cache);
    if (stretch_input) linearFree(stretch_input);
    pcm[0] = pcm[1] = NULL;
    scratch_cache = NULL;
    hot_cue_cache = NULL;
    loop_cache = NULL;
    stretch_input = NULL;
    stretch_exit();
    if (ndsp_available) ndspExit();
    ndsp_available = false;
    initialized = false;
}

bool audio_is_loaded(void) { return loaded; }
bool audio_get_transport_ms(uint32_t *milliseconds) {
    if (!milliseconds || !loaded || scratch_active || scrub_preview) return false;
    const u64 samples = current_transport_samples();
    *milliseconds = (uint32_t)((samples * 1000u) / sample_rate);
    return true;
}
const char *audio_status(void) { return status; }
