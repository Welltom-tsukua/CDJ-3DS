#pragma once

#include <stdbool.h>
#include <stdint.h>

bool audio_init(void);
bool audio_load_mp3(const char *path);
bool audio_load_m4a(const char *path);
void audio_set_paused(bool paused);
void audio_set_tempo(float percent);
void audio_set_pitch_bend(float percent);
bool audio_set_master_tempo(bool enabled);
void audio_set_duration_ms(uint32_t milliseconds);
void audio_set_gapless_delay_samples(uint32_t samples);
void audio_set_mp3_sample_rate(uint32_t samples_per_second);
void audio_set_mp3_seek_index(const uint32_t *offsets, const uint16_t *skips, uint32_t count);
/* MP3 Hot Cues are expanded while loading.  Triggering one then never waits
   for an SD read or an MP3 frame decode on the performance path. */
void audio_preload_hot_cues(const uint32_t *times, const uint8_t *numbers,
                            uint32_t count, uint32_t bpm_100, uint32_t restore_ms);
/* Use when an empty pad is assigned in the app.  It performs the one-time
   source decode now, so every later pad press has the same cache-only path. */
bool audio_cache_hot_cue(uint8_t number, uint32_t milliseconds, uint32_t restore_ms);
bool audio_cached_hot_cue_matches(uint8_t number, uint32_t milliseconds);
bool audio_trigger_preloaded_hot_cue(uint8_t number);
/* Loop boundaries live in the PCM producer, rather than the UI timer. */
void audio_set_loop(uint32_t start_ms, uint32_t end_ms, bool enabled);
/* Change loop length without a decoder seek.  Existing Loop-In PCM is reused
   when it covers the new first segment. */
void audio_resize_loop(uint32_t start_ms, uint32_t end_ms);
/* Pre-expand the loop start into PCM.  The boundary then avoids SD seeking. */
bool audio_prepare_loop_cache(uint32_t start_ms, uint32_t end_ms, uint32_t restore_ms);
/* Begin retaining PCM at Loop In while playback continues.  Loop Out can then
   reuse the prefix without a touch-time seek. */
void audio_prime_loop_in(uint32_t start_ms);
bool audio_arm_loop_capture(uint32_t start_ms, uint32_t end_ms);
bool audio_has_native_loop(void);
bool audio_seek_ms(uint32_t milliseconds);
bool audio_seek_hot_cue_ms(uint32_t milliseconds);
bool audio_scrub_ms(uint32_t milliseconds, bool reverse, float speed);
bool audio_begin_scrub(uint32_t milliseconds);
/* Returns true only when a PCM grain was actually queued.  The UI must not
   unpause an empty NDSP channel when the finger has moved outside the small
   prepared scratch window. */
bool audio_set_scrub_position(uint32_t milliseconds, bool reverse, float speed);
void audio_hold_scrub(void);
void audio_end_scrub(void);
void audio_update(void);
void audio_stop(void);
void audio_exit(void);
bool audio_is_loaded(void);
/* Actual PCM transport, re-anchored when NDSP consumes a buffer. */
bool audio_get_transport_ms(uint32_t *milliseconds);
const char *audio_status(void);
