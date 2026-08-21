#include <3ds.h>
#include <citro2d.h>
#include <dirent.h>
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "audio.h"

/* FAAD's stereo AAC reconstruction has two 1024-sample working buffers and
 * its caller also keeps a sizeable syntax-element frame on the stack.  The
 * libctru default is 32 KiB, which is not enough for M4A decoding. */
u32 __stacksize__ = 256 * 1024;

typedef enum { VIEW_DECK, VIEW_BROWSER } View;

static const int tempo_ranges[] = { 6, 10, 16, 100 };
#define MAX_TRACKS 128
#define RUNTIME_WAVE_POINTS 8192
#define MAX_BEAT_GRID 4096
#define MP3_SEEK_POINTS 16384
#define MP3_SEEK_STEP_MS 50
#define RUNTIME_HALF_WINDOW_MS 4000
typedef struct {
    char path[512];
    char title[144];
    char composer[80];
    char artist[80];
    char key[16];
    char artwork_path[128];
    u32 bpm_100;
    u32 duration_ms;
    u32 runtime_waveform_duration_ms;
    u32 audio_delay_samples, mp3_sample_rate;
    u32 track_number;
    u8 preview_waveform[400];
    u16 waveform_colours[400];
    u16 runtime_waveform[RUNTIME_WAVE_POINTS];
    u32 beat_times[MAX_BEAT_GRID];
    u8 beat_numbers[MAX_BEAT_GRID];
    u32 cue_times[16];
    u8 cue_numbers[16];
    u32 cue_colours[16];
    /* Loop end per A-H Hot Cue. UINT32_MAX means this cue is a plain cue. */
    u32 hotcue_loop_ends[8];
    u32 mp3_seek_offsets[MP3_SEEK_POINTS];
    u16 mp3_seek_skips[MP3_SEEK_POINTS];
    bool is_m4a;
} Track;
typedef struct {
    char path[512], title[160], composer[160], artist[120], key[16], artwork_path[64];
    u32 bpm_100, duration, runtime_waveform_duration_ms, audio_delay_samples, track_number, mp3_sample_rate;
    u8 preview_waveform[400];
    u16 waveform_colours[400];
    u16 runtime_waveform[RUNTIME_WAVE_POINTS];
    u32 beat_times[MAX_BEAT_GRID];
    u8 beat_numbers[MAX_BEAT_GRID];
    u32 cue_times[16];
    u8 cue_numbers[16];
    u32 cue_colours[16];
    u32 hotcue_loop_ends[8];
    u32 mp3_seek_offsets[MP3_SEEK_POINTS];
    u16 mp3_seek_skips[MP3_SEEK_POINTS];
} CacheTrack;
static Track library_tracks[MAX_TRACKS];
static int library_count;
static bool track_cue_overridden[MAX_TRACKS];

/* The rekordbox export is read-only to this homebrew. Cue edits live in a
   small sidecar keyed by exported media path, so they survive restart and a
   later PC cache rebuild without risking Pioneer PDB corruption. */
typedef struct {
    u32 path_hash;
    u32 cue_times[16];
    u8 cue_numbers[16];
    u32 cue_colours[16];
    u32 hotcue_loop_ends[8];
} CueOverride;
static C2D_TextBuf text_buf;
static C2D_Font pixel_font;
static C2D_Font japanese_font;
static u16 artwork_pixels[128 * 128];
static bool artwork_loaded;
static C3D_Tex artwork_texture;
static u16 startup_logo_pixels[256 * 128];
static C3D_Tex startup_logo_texture;
static bool startup_logo_texture_ready;
static u16 rekordbox_logo_pixels[256 * 128];
static C3D_Tex rekordbox_logo_texture;
static bool rekordbox_logo_texture_ready;
/* Citro2D uses top < bottom as its packed-texture 90-degree rotation flag.
   This is a direct, square GPU texture, so keep V in the normal 1-to-0 order. */
static const Tex3DS_SubTexture artwork_subtexture = { 128, 128, 0.0f, 1.0f, 1.0f, 0.0f };
static const Tex3DS_SubTexture startup_logo_subtexture = { 256, 128, 0.0f, 1.0f, 1.0f, 0.0f };
static const C2D_Image artwork_image = { &artwork_texture, &artwork_subtexture };
static const C2D_Image startup_logo_image = { &startup_logo_texture, &startup_logo_subtexture };
static const C2D_Image rekordbox_logo_image = { &rekordbox_logo_texture, &startup_logo_subtexture };
static bool artwork_texture_ready;
static u16 browser_artwork_pixels[3][128 * 128];
static C3D_Tex browser_artwork_textures[3];
static bool browser_artwork_loaded[3], browser_artwork_texture_ready[3];
static int browser_artwork_track[3] = { -1, -1, -1 };
static int browser_artwork_refresh_slot;

/* The runtime analyser is 25 ms / point at its native density.  Keep a
   physical 320-pixel source image and shift that image as transport moves;
   this avoids the two adjacent screen regions that used to shimmer when each
   frame independently chose a fractional analyser sample. */
static u16 runtime_wave_pixels[320];
static const Track *runtime_wave_pixels_track;
static int runtime_wave_pixels_scroll = -0x7fffffff;
static float runtime_wave_pixels_stride = -1.0f;
static const float runtime_wave_zoom_levels[] = { 0.50f, 0.75f, 1.00f, 1.50f, 2.00f };
static int runtime_wave_zoom_index = 2;

#define PIXEL_SCALE 1.24f

#define RGB(r,g,b) ((u32)(r) | ((u32)(g) << 8) | ((u32)(b) << 16) | 0xFF000000u)
static const u32 BLACK = RGB(0, 0, 0), INNER = RGB(24, 35, 49);
static const u32 BORDER = RGB(76, 92, 108), TEXT = RGB(235, 239, 242), MUTED = RGB(142, 154, 166);
static const u32 CYAN = RGB(0, 194, 255), RED = RGB(238, 91, 104), AMBER = RGB(255, 177, 67);
static const u32 GREEN = RGB(45, 218, 105), BLUE = RGB(70, 156, 255), GREY = RGB(148, 158, 170);
static const u32 WAVE_DARK = RGB(20, 57, 73);
static const u32 PAD_EMPTY = RGB(102, 112, 124);

static bool has_audio_extension(const char *name) {
    const char *dot = strrchr(name, '.');
    if (!dot) return false;
    return strcmp(dot, ".mp3") == 0 || strcmp(dot, ".MP3") == 0 ||
           strcmp(dot, ".m4a") == 0 || strcmp(dot, ".M4A") == 0;
}

/* Some ID3/M4A writers retain an UTF-8 BOM at the beginning of a tag.  Citro2D
   treats that invisible first scalar as a glyph, which looks like only the
   title's first character is mojibake.  Strip it before any font selection. */
static void strip_utf8_bom(char *value) {
    if (!value) return;
    if ((u8)value[0] == 0xef && (u8)value[1] == 0xbb && (u8)value[2] == 0xbf)
        memmove(value, value + 3, strlen(value + 3) + 1);
}

static u32 cue_path_hash(const char *path) {
    u32 hash = 2166136261u;
    if (!path) return hash;
    for (; *path; ++path) {
        hash ^= (u8)*path;
        hash *= 16777619u;
    }
    return hash;
}

static void apply_cue_override(const CueOverride *override) {
    if (!override) return;
    for (int i = 0; i < library_count; ++i) {
        Track *track = &library_tracks[i];
        if (cue_path_hash(track->path) != override->path_hash) continue;
        memcpy(track->cue_times, override->cue_times, sizeof(track->cue_times));
        memcpy(track->cue_numbers, override->cue_numbers, sizeof(track->cue_numbers));
        memcpy(track->cue_colours, override->cue_colours, sizeof(track->cue_colours));
        memcpy(track->hotcue_loop_ends, override->hotcue_loop_ends,
               sizeof(track->hotcue_loop_ends));
        track_cue_overridden[i] = true;
        return;
    }
}

static void load_cue_overrides(void) {
    static const char magic[8] = "3DCUE02";
    FILE *file = fopen("sdmc:/3ds/3ds_one_deck/cache/cue-overrides.rbd", "rb");
    char stored_magic[8];
    u32 count;
    if (!file) return;
    if (fread(stored_magic, 1, sizeof(stored_magic), file) != sizeof(stored_magic) ||
        memcmp(stored_magic, magic, sizeof(magic)) != 0 ||
        fread(&count, sizeof(count), 1, file) != 1) {
        fclose(file); return;
    }
    for (u32 i = 0; i < count && i < MAX_TRACKS; ++i) {
        CueOverride override;
        if (fread(&override, sizeof(override), 1, file) != 1) break;
        apply_cue_override(&override);
    }
    fclose(file);
}

static void save_cue_overrides(void) {
    static const char magic[8] = "3DCUE02";
    FILE *file = fopen("sdmc:/3ds/3ds_one_deck/cache/cue-overrides.rbd", "wb");
    u32 count = 0;
    if (!file) return;
    for (int i = 0; i < library_count; ++i) if (track_cue_overridden[i]) ++count;
    if (fwrite(magic, 1, sizeof(magic), file) != sizeof(magic) ||
        fwrite(&count, sizeof(count), 1, file) != 1) {
        fclose(file); return;
    }
    for (int i = 0; i < library_count; ++i) {
        if (!track_cue_overridden[i]) continue;
        CueOverride override;
        override.path_hash = cue_path_hash(library_tracks[i].path);
        memcpy(override.cue_times, library_tracks[i].cue_times, sizeof(override.cue_times));
        memcpy(override.cue_numbers, library_tracks[i].cue_numbers, sizeof(override.cue_numbers));
        memcpy(override.cue_colours, library_tracks[i].cue_colours, sizeof(override.cue_colours));
        memcpy(override.hotcue_loop_ends, library_tracks[i].hotcue_loop_ends,
               sizeof(override.hotcue_loop_ends));
        if (fwrite(&override, sizeof(override), 1, file) != 1) break;
    }
    fflush(file);
    fclose(file);
}

static void mark_cue_override(const Track *track) {
    const ptrdiff_t index = track - library_tracks;
    if (index >= 0 && index < library_count) track_cue_overridden[index] = true;
}

static void initialise_track(Track *track) {
    if (!track) return;
    memset(track, 0, sizeof(*track));
    memset(track->cue_times, 0xff, sizeof(track->cue_times));
    memset(track->hotcue_loop_ends, 0xff, sizeof(track->hotcue_loop_ends));
}

static bool load_rekordbox_cache(void) {
    char magic[8]; u32 count;
    FILE *file = fopen("sdmc:/3ds/3ds_one_deck/cache/library.rbd", "rb");
    if (!file) return false;
    if (fread(magic, 1, sizeof(magic), file) != sizeof(magic) ||
        memcmp(magic, "RB3D15\0\0", 8) != 0 || fread(&count, sizeof(count), 1, file) != 1) {
        fclose(file); return false;
    }
    for (u32 i = 0; i < count && library_count < MAX_TRACKS; ++i) {
        CacheTrack cached;
        if (fread(&cached, sizeof(cached), 1, file) != 1) break;
        Track *track = &library_tracks[library_count++];
        initialise_track(track);
        snprintf(track->path, sizeof(track->path), "%s", cached.path);
        snprintf(track->title, sizeof(track->title), "%s", cached.title);
        snprintf(track->composer, sizeof(track->composer), "%s", cached.composer);
        snprintf(track->artist, sizeof(track->artist), "%s", cached.artist);
        snprintf(track->key, sizeof(track->key), "%s", cached.key);
        strip_utf8_bom(track->title);
        strip_utf8_bom(track->composer);
        strip_utf8_bom(track->artist);
        snprintf(track->artwork_path, sizeof(track->artwork_path), "sdmc:/3ds/3ds_one_deck/%s", cached.artwork_path);
        track->bpm_100 = cached.bpm_100;
        track->runtime_waveform_duration_ms = cached.runtime_waveform_duration_ms;
        /* ANLZ high-resolution waveforms and their beat grid share this
           millisecond timebase. PDB's duration is only whole seconds, so
           using it here makes grid and waveform progressively diverge. */
        track->duration_ms = cached.runtime_waveform_duration_ms ?
            cached.runtime_waveform_duration_ms : cached.duration * 1000;
        track->audio_delay_samples = cached.audio_delay_samples;
        track->track_number = cached.track_number;
        track->mp3_sample_rate = cached.mp3_sample_rate;
        memcpy(track->preview_waveform, cached.preview_waveform, sizeof(track->preview_waveform));
        memcpy(track->waveform_colours, cached.waveform_colours, sizeof(track->waveform_colours));
        memcpy(track->runtime_waveform, cached.runtime_waveform, sizeof(track->runtime_waveform));
        memcpy(track->beat_times, cached.beat_times, sizeof(track->beat_times));
        memcpy(track->beat_numbers, cached.beat_numbers, sizeof(track->beat_numbers));
        memcpy(track->cue_times, cached.cue_times, sizeof(track->cue_times));
        memcpy(track->cue_numbers, cached.cue_numbers, sizeof(track->cue_numbers));
        memcpy(track->cue_colours, cached.cue_colours, sizeof(track->cue_colours));
        memcpy(track->hotcue_loop_ends, cached.hotcue_loop_ends,
               sizeof(track->hotcue_loop_ends));
        memcpy(track->mp3_seek_offsets, cached.mp3_seek_offsets, sizeof(track->mp3_seek_offsets));
        memcpy(track->mp3_seek_skips, cached.mp3_seek_skips, sizeof(track->mp3_seek_skips));
        const char *dot = strrchr(track->path, '.');
        track->is_m4a = dot && (strcmp(dot, ".m4a") == 0 || strcmp(dot, ".M4A") == 0);
    }
    fclose(file);
    return library_count > 0;
}

/* The PICA200 texture layout is an 8x8 Morton-order tile grid.  Keeping this
 * upload here lets the cover remain a genuine 128x128 image while drawing it
 * with one GPU primitive, rather than thousands of C2D rectangles. */
static unsigned int texture_tile_offset(unsigned int x, unsigned int y, unsigned int width) {
    const unsigned int morton = (x & 1u) | ((y & 1u) << 1) |
        ((x & 2u) << 1) | ((y & 2u) << 2) |
        ((x & 4u) << 2) | ((y & 4u) << 3);
    return (y & ~7u) * width + (x & ~7u) * 8u + morton;
}

static bool load_rgb565_logo(const char *path, u16 *pixels, C3D_Tex *texture) {
    FILE *file = fopen(path, "rb");
    if (!file) return false;
    const bool read = fread(pixels, sizeof(u16), 256 * 128, file) == 256 * 128;
    fclose(file);
    if (!read || !C3D_TexInit(texture, 256, 128, GPU_RGB565)) return false;
    u16 *destination = (u16 *)C3D_Tex2DGetImagePtr(texture, 0, NULL);
    for (unsigned int y = 0; y < 128; ++y)
        for (unsigned int x = 0; x < 256; ++x)
            destination[texture_tile_offset(x, y, 256)] = pixels[y * 256u + x];
    C3D_TexSetFilter(texture, GPU_LINEAR, GPU_LINEAR);
    C3D_TexSetWrap(texture, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
    C3D_TexFlush(texture);
    return true;
}

static void load_startup_logos(void) {
    startup_logo_texture_ready = load_rgb565_logo("romfs:/PioneerDJLogo.rgb565",
                                                   startup_logo_pixels, &startup_logo_texture);
    rekordbox_logo_texture_ready = load_rgb565_logo("romfs:/rekordboxLogo.rgb565",
                                                     rekordbox_logo_pixels, &rekordbox_logo_texture);
}

static void load_artwork(const Track *track) {
    artwork_loaded = false;
    if (!track || !track->artwork_path[0]) return;
    FILE *file = fopen(track->artwork_path, "rb");
    if (!file) return;
    artwork_loaded = fread(artwork_pixels, sizeof(u16), 128 * 128, file) == 128 * 128;
    fclose(file);
    if (!artwork_loaded) return;
    if (!artwork_texture_ready) {
        if (!C3D_TexInit(&artwork_texture, 128, 128, GPU_RGB565)) {
            artwork_loaded = false;
            return;
        }
        C3D_TexSetFilter(&artwork_texture, GPU_LINEAR, GPU_LINEAR);
        C3D_TexSetWrap(&artwork_texture, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
        artwork_texture_ready = true;
    }
    u16 *destination = (u16 *)C3D_Tex2DGetImagePtr(&artwork_texture, 0, NULL);
    for (unsigned int y = 0; y < 128; ++y)
        for (unsigned int x = 0; x < 128; ++x)
            destination[texture_tile_offset(x, y, 128)] = artwork_pixels[y * 128u + x];
    C3D_TexFlush(&artwork_texture);
}

static void draw_artwork(float x, float y) {
    if (artwork_loaded && artwork_texture_ready)
        C2D_DrawImageAt(artwork_image, x, y, 0.0f, NULL, 82.0f / 128.0f, 82.0f / 128.0f);
}

static void load_browser_artwork(int slot, int track_index) {
    if (slot < 0 || slot >= 3 || track_index < 0 || track_index >= library_count) return;
    if (browser_artwork_track[slot] == track_index) return;
    browser_artwork_track[slot] = track_index;
    browser_artwork_loaded[slot] = false;
    const Track *track = &library_tracks[track_index];
    if (!track->artwork_path[0]) return;
    FILE *file = fopen(track->artwork_path, "rb");
    if (!file) return;
    browser_artwork_loaded[slot] = fread(browser_artwork_pixels[slot], sizeof(u16), 128 * 128, file) == 128 * 128;
    fclose(file);
    if (!browser_artwork_loaded[slot]) return;
    if (!browser_artwork_texture_ready[slot]) {
        if (!C3D_TexInit(&browser_artwork_textures[slot], 128, 128, GPU_RGB565)) {
            browser_artwork_loaded[slot] = false;
            return;
        }
        C3D_TexSetFilter(&browser_artwork_textures[slot], GPU_LINEAR, GPU_LINEAR);
        C3D_TexSetWrap(&browser_artwork_textures[slot], GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
        browser_artwork_texture_ready[slot] = true;
    }
    u16 *destination = (u16 *)C3D_Tex2DGetImagePtr(&browser_artwork_textures[slot], 0, NULL);
    for (unsigned int y = 0; y < 128; ++y)
        for (unsigned int x = 0; x < 128; ++x)
            destination[texture_tile_offset(x, y, 128)] = browser_artwork_pixels[slot][y * 128u + x];
    C3D_TexFlush(&browser_artwork_textures[slot]);
}

static void draw_browser_artwork(int slot, float x, float y, float size) {
    if (slot < 0 || slot >= 3 || !browser_artwork_loaded[slot] || !browser_artwork_texture_ready[slot]) return;
    const C2D_Image image = { &browser_artwork_textures[slot], &artwork_subtexture };
    C2D_DrawImageAt(image, x, y, 0.0f, NULL, size / 128.0f, size / 128.0f);
}

static u32 be32(const u8 *value) {
    return ((u32)value[0] << 24) | ((u32)value[1] << 16) |
           ((u32)value[2] << 8) | value[3];
}

static u32 synchsafe32(const u8 *value) {
    return ((u32)(value[0] & 0x7f) << 21) | ((u32)(value[1] & 0x7f) << 14) |
           ((u32)(value[2] & 0x7f) << 7) | (value[3] & 0x7f);
}

static bool append_utf8(char *destination, size_t capacity, size_t *written, u32 codepoint) {
    if (codepoint <= 0x7f) {
        if (*written + 1 >= capacity) return false;
        destination[(*written)++] = (char)codepoint;
    } else if (codepoint <= 0x7ff) {
        if (*written + 2 >= capacity) return false;
        destination[(*written)++] = (char)(0xc0 | (codepoint >> 6));
        destination[(*written)++] = (char)(0x80 | (codepoint & 0x3f));
    } else if (codepoint <= 0xffff) {
        if (*written + 3 >= capacity) return false;
        destination[(*written)++] = (char)(0xe0 | (codepoint >> 12));
        destination[(*written)++] = (char)(0x80 | ((codepoint >> 6) & 0x3f));
        destination[(*written)++] = (char)(0x80 | (codepoint & 0x3f));
    } else if (codepoint <= 0x10ffff) {
        if (*written + 4 >= capacity) return false;
        destination[(*written)++] = (char)(0xf0 | (codepoint >> 18));
        destination[(*written)++] = (char)(0x80 | ((codepoint >> 12) & 0x3f));
        destination[(*written)++] = (char)(0x80 | ((codepoint >> 6) & 0x3f));
        destination[(*written)++] = (char)(0x80 | (codepoint & 0x3f));
    } else return false;
    return true;
}

static void copy_metadata_text(char *destination, size_t capacity, const u8 *source, size_t length) {
    if (!capacity) return;
    destination[0] = '\0';
    if (!source || length < 2) return;
    const int encoding = source[0];
    size_t written = 0;
    if (encoding == 0 || encoding == 3) { /* ISO-8859-1 or UTF-8 */
        for (size_t i = 1; i < length && source[i] && written + 1 < capacity; ++i)
            destination[written++] = (char)source[i];
    } else { /* UTF-16 ID3 tags: preserve Japanese rather than replacing it with '?'. */
        size_t start = 1;
        bool little_endian = true;
        if (length >= 3 && source[1] == 0xfe && source[2] == 0xff) { little_endian = false; start = 3; }
        else if (length >= 3 && source[1] == 0xff && source[2] == 0xfe) start = 3;
        for (size_t i = start; i + 1 < length && written + 1 < capacity; i += 2) {
            u16 code = little_endian ? (u16)(source[i] | (source[i + 1] << 8))
                                      : (u16)((source[i] << 8) | source[i + 1]);
            if (!code) break;
            u32 codepoint = code;
            if (code >= 0xd800 && code <= 0xdbff && i + 3 < length) {
                const u16 low = little_endian ? (u16)(source[i + 2] | (source[i + 3] << 8))
                                               : (u16)((source[i + 2] << 8) | source[i + 3]);
                if (low >= 0xdc00 && low <= 0xdfff) {
                    codepoint = 0x10000u + (((u32)code - 0xd800u) << 10) + ((u32)low - 0xdc00u);
                    i += 2;
                }
            }
            if (!append_utf8(destination, capacity, &written, codepoint)) break;
        }
    }
    destination[written] = '\0';
}

static bool read_id3_composer(const char *path, char *destination, size_t capacity) {
    u8 header[10], frame[10], payload[512];
    FILE *file = fopen(path, "rb");
    if (!file) return false;
    const bool valid = fread(header, 1, sizeof(header), file) == sizeof(header) &&
                       memcmp(header, "ID3", 3) == 0 && (header[3] == 3 || header[3] == 4);
    if (!valid) { fclose(file); return false; }
    const long end = 10 + (long)synchsafe32(header + 6);
    while (ftell(file) + 10 <= end && fread(frame, 1, sizeof(frame), file) == sizeof(frame)) {
        if (frame[0] == 0) break;
        const u32 size = header[3] == 4 ? synchsafe32(frame + 4) : be32(frame + 4);
        const long next = ftell(file) + (long)size;
        if (size == 0 || next > end) break;
        if (memcmp(frame, "TCOM", 4) == 0) {
            const size_t wanted = size < sizeof(payload) ? size : sizeof(payload);
            if (fread(payload, 1, wanted, file) == wanted) copy_metadata_text(destination, capacity, payload, wanted);
            fclose(file);
            return destination[0] != '\0';
        }
        fseek(file, next, SEEK_SET);
    }
    fclose(file);
    return false;
}

static bool read_m4a_composer(const char *path, char *destination, size_t capacity) {
    const size_t window = 1024 * 1024;
    u8 *data = malloc(window);
    FILE *file = data ? fopen(path, "rb") : NULL;
    if (!file) { free(data); return false; }
    fseek(file, 0, SEEK_END);
    long end = ftell(file);
    const long start = end > (long)window ? end - (long)window : 0;
    fseek(file, start, SEEK_SET);
    const size_t length = fread(data, 1, window, file);
    fclose(file);
    for (size_t i = 0; i + 24 < length; ++i) {
        if (data[i] != 0xa9 || memcmp(data + i + 1, "wrt", 3) != 0) continue;
        for (size_t atom = i + 4; atom + 16 < length && atom < i + 512; ++atom) {
            if (memcmp(data + atom + 4, "data", 4) != 0) continue;
            const u32 atom_size = be32(data + atom);
            if (atom_size < 17 || atom + atom_size > length) break;
            copy_metadata_text(destination, capacity, data + atom + 16, atom_size - 16);
            free(data);
            return destination[0] != '\0';
        }
    }
    free(data);
    return false;
}

static void add_track(const char *path) {
    if (library_count >= MAX_TRACKS) return;
    Track *track = &library_tracks[library_count];
    initialise_track(track);
    const char *base = strrchr(path, '/');
    char parent[512];
    base = base ? base + 1 : path;
    snprintf(track->path, sizeof(track->path), "%s", path);
    snprintf(track->title, sizeof(track->title), "%.143s", base);
    char *dot = strrchr(track->title, '.');
    if (dot) *dot = '\0';
    snprintf(parent, sizeof(parent), "%s", path);
    char *slash = strrchr(parent, '/');
    if (slash) *slash = '\0';                 /* album directory */
    track->is_m4a = strstr(base, ".m4a") != NULL || strstr(base, ".M4A") != NULL;
    snprintf(track->composer, sizeof(track->composer), "UNKNOWN COMPOSER");
    if (track->is_m4a) read_m4a_composer(path, track->composer, sizeof(track->composer));
    else read_id3_composer(path, track->composer, sizeof(track->composer));
    ++library_count;
}

/* Azahar has its own virtual SD root.  These entries keep the Browser visible
   there; real hardware always replaces them with the scanned /Contents files. */
static void add_preview_track(const char *title, const char *composer, bool is_m4a) {
    if (library_count >= MAX_TRACKS) return;
    Track *track = &library_tracks[library_count++];
    initialise_track(track);
    snprintf(track->title, sizeof(track->title), "%s", title);
    snprintf(track->composer, sizeof(track->composer), "%s", composer);
    track->is_m4a = is_m4a;
}

static bool library_has_path(const char *path) {
    for (int i = 0; i < library_count; ++i)
        if (strcmp(library_tracks[i].path, path) == 0) return true;
    return false;
}

static bool is_ascii_path(const char *path) {
    for (const u8 *cursor = (const u8 *)path; *cursor; ++cursor)
        if (*cursor >= 0x80) return false;
    return true;
}

static void scan_library(const char *directory, int depth) {
    if (depth > 3 || library_count >= MAX_TRACKS) return;
    DIR *dir = opendir(directory);
    if (!dir) return;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL && library_count < MAX_TRACKS) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        char path[512];
        struct stat st;
        snprintf(path, sizeof(path), "%s/%s", directory, entry->d_name);
        if (stat(path, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) scan_library(path, depth + 1);
        else if (has_audio_extension(entry->d_name) && is_ascii_path(path) && !library_has_path(path))
            add_track(path);
    }
    closedir(dir);
}

/* Use the Japanese 3DS system font first. It is the same family used by the
   HOME menu and, unlike a generated per-library subset, it cannot miss a
   newly exported title or composer. */
static C2D_Font font_for(const char *value) {
    (void)value;
    /* The bundled Yu Gothic Bold subset contains the ASCII UI glyphs too.
       Using it for both Japanese and Latin text avoids a thin mixed-font
       screen and keeps punctuation such as 【】 reliable on real hardware. */
    return japanese_font ? japanese_font : pixel_font;
}

static void text(float x, float y, float scale, u32 colour, const char *value) {
    C2D_Text item;
    C2D_TextFontParse(&item, font_for(value), text_buf, value);
    C2D_TextOptimize(&item);
    const float draw_scale = scale * PIXEL_SCALE;
    C2D_DrawText(&item, C2D_WithColor, x, y, 0.0f, draw_scale, draw_scale, colour);
}

static void text_scaled(float x, float y, float scale_x, float scale_y, u32 colour, const char *value) {
    C2D_Text item;
    C2D_TextFontParse(&item, font_for(value), text_buf, value);
    C2D_TextOptimize(&item);
    C2D_DrawText(&item, C2D_WithColor, x, y, 0.0f, scale_x * PIXEL_SCALE, scale_y * PIXEL_SCALE, colour);
}

static void text_center(float x, float width, float y, float scale, u32 colour, const char *value) {
    C2D_Text item;
    C2D_TextFontParse(&item, font_for(value), text_buf, value);
    C2D_TextOptimize(&item);
    C2D_DrawText(&item, C2D_WithColor | C2D_AlignCenter, x + width * 0.5f, y, 0.0f,
                 scale * PIXEL_SCALE, scale * PIXEL_SCALE, colour);
}

static void text_right(float right, float y, float scale, u32 colour, const char *value) {
    C2D_Text item;
    float width;
    C2D_TextFontParse(&item, font_for(value), text_buf, value);
    C2D_TextOptimize(&item);
    C2D_TextGetDimensions(&item, scale * PIXEL_SCALE, scale * PIXEL_SCALE, &width, NULL);
    C2D_DrawText(&item, C2D_WithColor, right - width, y, 0.0f, scale * PIXEL_SCALE, scale * PIXEL_SCALE, colour);
}

/* The deck clock is drawn as two sizes, but it must still behave like one
   centred number. Measuring both glyph runs removes the visible gap. */
static void text_time_center(float centre, float y, const char *seconds, const char *milliseconds) {
    C2D_Text whole, fraction;
    float whole_width, fraction_width;
    const float whole_scale = 0.84f * PIXEL_SCALE;
    const float fraction_scale = 0.42f * PIXEL_SCALE;
    C2D_TextFontParse(&whole, font_for(seconds), text_buf, seconds);
    C2D_TextOptimize(&whole);
    C2D_TextFontParse(&fraction, font_for(milliseconds), text_buf, milliseconds);
    C2D_TextOptimize(&fraction);
    C2D_TextGetDimensions(&whole, whole_scale, whole_scale, &whole_width, NULL);
    C2D_TextGetDimensions(&fraction, fraction_scale, fraction_scale, &fraction_width, NULL);
    const float x = centre - (whole_width + 2.0f + fraction_width) * 0.5f;
    C2D_DrawText(&whole, C2D_WithColor, x, y, 0.0f, whole_scale, whole_scale, TEXT);
    C2D_DrawText(&fraction, C2D_WithColor, x + whole_width + 2.0f, y + 11.0f,
                 0.0f, fraction_scale, fraction_scale, TEXT);
}

static void box(float x, float y, float w, float h, u32 fill, u32 outline) {
    C2D_DrawRectSolid(x, y, 0.0f, w, h, fill);
    C2D_DrawLine(x, y, outline, x + w, y, outline, 1.0f, 0.0f);
    C2D_DrawLine(x, y + h - 1, outline, x + w, y + h - 1, outline, 1.0f, 0.0f);
    C2D_DrawLine(x, y, outline, x, y + h, outline, 1.0f, 0.0f);
    C2D_DrawLine(x + w - 1, y, outline, x + w - 1, y + h, outline, 1.0f, 0.0f);
}

/* The playback marker stays in the centre for the runtime lane, as in the PC player. */
static void waveform(float left, int width, int y, int height, float playhead, bool runtime, bool vinyl, const u8 *data, const u16 *colours) {
    if (!data) return;
    const float centre = runtime ? left + width * 0.5f : left + playhead * (width - 1);
    const float mid = y + height * 0.5f;
    const u32 colour = vinyl ? AMBER : CYAN;
    C2D_DrawLine(left, mid, WAVE_DARK, left + width, mid, WAVE_DARK, 1.0f, 0.0f);
    if (data) for (int x = 0; x < width; ++x) {
        const int point = runtime ? (int)((playhead + (x - width / 2) / (float)width * 0.15f) * 400.0f)
                                  : x * 400 / width;
        if (point < 0 || point >= 400) continue;
        const float amplitude = (data[point] & 0x1f) * height / 64.0f;
        u32 wave_colour = colour;
        if (colours && colours[point]) {
            const u16 rgb565 = colours[point];
            wave_colour = RGB(((rgb565 >> 11) & 31) * 255 / 31, ((rgb565 >> 5) & 63) * 255 / 63,
                              (rgb565 & 31) * 255 / 31);
        }
        C2D_DrawLine(left + x, mid - amplitude, wave_colour, left + x, mid + amplitude, wave_colour, 1.0f, 0.0f);
    }
    C2D_DrawLine(centre, (float)y, RED, centre, (float)(y + height), RED, 2.0f, 0.0f);
}

static float tempo_visual_factor(float tempo) {
    float factor = 1.0f + tempo / 100.0f;
    return factor < 0.025f ? 0.025f : factor;
}

/* Match the 2deck renderer: waveform data and every moving marker use one
   integer pixel scroll origin.  Independently rounding (time - playhead) in
   each element made columns occasionally choose neighbouring samples and
   looked like the waveform was vibrating. */
static float runtime_pixels_per_ms(float visual_factor) {
    return runtime_wave_zoom_levels[runtime_wave_zoom_index] *
        320.0f / (RUNTIME_HALF_WINDOW_MS * 2.0f * visual_factor);
}

static int runtime_waveform_points(const Track *track) {
    if (!track || !track->runtime_waveform_duration_ms) return RUNTIME_WAVE_POINTS;
    const u32 points = (track->runtime_waveform_duration_ms + 24) / 25;
    return points > RUNTIME_WAVE_POINTS ? RUNTIME_WAVE_POINTS : (int)points;
}

static int runtime_scroll_px(int playhead, float visual_factor) {
    return (int)lroundf(playhead * runtime_pixels_per_ms(visual_factor));
}

static float runtime_x_for_time(u32 time_ms, int playhead, float visual_factor) {
    const float pixels_per_ms = runtime_pixels_per_ms(visual_factor);
    /* Waveform pixels use the integer scroll origin below. Markers must use
       the same origin; otherwise zoomed views round the wave and grid on
       different frames, which reads as a local waveform vibration. */
    return 160.0f + (float)time_ms * pixels_per_ms -
        (float)runtime_scroll_px(playhead, visual_factor);
}

static float runtime_time_for_x(float x, int playhead, float visual_factor) {
    const float pixels_per_ms = runtime_pixels_per_ms(visual_factor);
    return ((float)runtime_scroll_px(playhead, visual_factor) + x - 160.0f) / pixels_per_ms;
}

/* The UI has one canonical source-time clock.  It is anchored only when a
   track starts, pauses, seeks, or its tempo changes.  Crucially, it does not
   add rounded per-frame deltas: that accumulated truncation was the cause of
   the beat grid slowly drifting away from the actual deck position. */
static int timeline_position(int anchor_ms, u64 anchor_tick, float tempo, int duration, u64 now) {
    /* Match the DSP's minimum rate exactly.  At WIDE/-100 the former zero
       timeline while audio still had a non-zero emergency rate caused an
       immediate waveform/grid divergence. */
    const double speed = fmax(0.025, 1.0 + (double)tempo / 100.0);
    const int position = anchor_ms + (int)llround((double)(now - anchor_tick) * speed);
    if (position < 0) return 0;
    if (position > duration) return duration;
    return position;
}

static u32 runtime_colour(u16 point) {
    const u8 red = (u8)(((point >> 13) & 7) * 255 / 7);
    const u8 green = (u8)(((point >> 10) & 7) * 255 / 7);
    const u8 blue = (u8)(((point >> 7) & 7) * 255 / 7);
    return (red || green || blue) ? RGB(red, green, blue) : CYAN;
}

static u32 tempo_range_colour(int range) {
    if (range == 6) return GREEN;
    if (range == 10) return AMBER;
    if (range == 16) return TEXT;
    return RED;
}

static void draw_runtime_waveform(const Track *track, int playhead, int duration, int y, int height,
                                  float visual_factor) {
    if (!track || duration <= 0) return;
    const float middle = y + height * 0.5f;
    C2D_DrawLine(0, middle, WAVE_DARK, 320, middle, WAVE_DARK, 1.0f, 0.0f);
    const float pixels_per_ms = runtime_pixels_per_ms(visual_factor);
    const int waveform_points = runtime_waveform_points(track);
    const u32 waveform_duration = track->runtime_waveform_duration_ms ?
        track->runtime_waveform_duration_ms : (u32)duration;
    /* Number of analyser points represented by exactly one physical pixel.
       It is the inverse of the transform shared by grid/cue/touch input. */
    const float sample_stride = (float)waveform_points /
        ((float)waveform_duration * pixels_per_ms);
    const int scroll = runtime_scroll_px(playhead, visual_factor);
    const bool scroll_one_right = runtime_wave_pixels_track == track &&
        fabsf(sample_stride - runtime_wave_pixels_stride) < 0.0001f &&
        scroll == runtime_wave_pixels_scroll + 1;
    const bool scroll_one_left = runtime_wave_pixels_track == track &&
        fabsf(sample_stride - runtime_wave_pixels_stride) < 0.0001f &&
        scroll == runtime_wave_pixels_scroll - 1;
    if (scroll_one_right) {
        memmove(runtime_wave_pixels, runtime_wave_pixels + 1, 319 * sizeof(u16));
        const int source = (int)lroundf(((float)(scroll + 159) / pixels_per_ms) *
                                        waveform_points / waveform_duration);
        runtime_wave_pixels[319] = source >= 0 && source < waveform_points ?
            track->runtime_waveform[source] : 0;
    } else if (scroll_one_left) {
        memmove(runtime_wave_pixels + 1, runtime_wave_pixels, 319 * sizeof(u16));
        const int source = (int)lroundf(((float)(scroll - 160) / pixels_per_ms) *
                                        waveform_points / waveform_duration);
        runtime_wave_pixels[0] = source >= 0 && source < waveform_points ?
            track->runtime_waveform[source] : 0;
    } else if (runtime_wave_pixels_track != track || runtime_wave_pixels_scroll != scroll ||
               fabsf(sample_stride - runtime_wave_pixels_stride) >= 0.0001f) {
        for (int x = 0; x < 320; ++x) {
            const int source = (int)lroundf(((float)(scroll + x - 160) / pixels_per_ms) *
                                            waveform_points / waveform_duration);
            runtime_wave_pixels[x] = source >= 0 && source < waveform_points ?
                track->runtime_waveform[source] : 0;
        }
    }
    runtime_wave_pixels_track = track;
    runtime_wave_pixels_scroll = scroll;
    runtime_wave_pixels_stride = sample_stride;
    for (int x = 0; x < 320; ++x) {
        const u16 value = runtime_wave_pixels[x];
        const float amplitude = ((value >> 2) & 31) * (height * 0.46f / 31.0f);
        C2D_DrawLine((float)x, middle - amplitude, runtime_colour(value), (float)x, middle + amplitude,
                     runtime_colour(value), 1.0f, 0.0f);
    }
    C2D_DrawLine(160, (float)y, RED, 160, (float)(y + height), RED, 2.0f, 0.0f);
}

static void draw_overview_cues(const Track *track, int duration, float left, float width,
                               float waveform_y, int cue) {
    if (!track || duration <= 0) return;
    /* Memory cues use the device-library colour and live immediately above the
       overview.  Hot cues are deliberately drawn second so they stay on top
       when both cue types share the exact same time. */
    for (int i = 0; i < 16 && track->cue_times[i] != 0xffffffff; ++i) {
        if (track->cue_numbers[i]) continue;
        const float x = left + (float)track->cue_times[i] * (width - 1.0f) / duration;
        if (x < left - 4.0f || x > left + width + 4.0f) continue;
        /* Device exports encode their generic memory-cue colour as amber.
           Treat that generic value as the deck default red, but retain any
           deliberately assigned rekordbox/device colour. */
        const u32 colour = !track->cue_colours[i] || track->cue_colours[i] == AMBER ?
            RED : track->cue_colours[i];
        C2D_DrawTriangle(x - 3.5f, waveform_y - 8, colour, x + 3.5f, waveform_y - 8,
                         colour, x, waveform_y - 1, colour, 0.0f);
    }
    for (int i = 0; i < 16 && track->cue_times[i] != 0xffffffff; ++i) {
        if (!track->cue_numbers[i]) continue;
        const float x = left + (float)track->cue_times[i] * (width - 1.0f) / duration;
        if (x < left - 5.0f || x > left + width + 5.0f) continue;
        const u32 colour = track->cue_colours[i] ? track->cue_colours[i] : GREEN;
        const char label[2] = { (char)('A' + ((track->cue_numbers[i] - 1) % 8)), '\0' };
        /* This is the only cue marker visible while looking at a complete
           song.  Make the pad letter readable at the 3DS's native scale. */
        C2D_DrawRectSolid(x - 7.0f, waveform_y - 19, 0.0f, 14, 14, colour);
        text(x - 4.0f, waveform_y - 17, 0.25f, BLACK, label);
    }
    const float cue_x = left + (float)cue * (width - 1.0f) / duration;
    if (cue_x >= left - 4.0f && cue_x <= left + width + 4.0f)
        C2D_DrawTriangle(cue_x, waveform_y + 52, AMBER,
                         cue_x - 4, waveform_y + 59, AMBER,
                         cue_x + 4, waveform_y + 59, AMBER, 0.0f);
}

static void status_box(float x, float y, float w, const char *label, const char *value, u32 colour) {
    box(x, y, w, value[0] ? 25.0f : 17.0f, BLACK, colour);
    text_center(x, w, y + 2, 0.26f, MUTED, label);
    if (value[0]) text_center(x, w, y + 11, 0.40f, TEXT, value);
}

static void top_menu(int menu) {
    static const char *const labels[] = { "HOT CUE", "MEMORY CUE", "BEAT LOOP", "BEAT JUMP", "SETTING" };
    static const u32 colours[] = { GREEN, RED, AMBER, BLUE, GREY };
    for (int i = 0; i < 5; ++i) {
        const float x = 3 + i * 79.0f;
        box(x, 3, 75, 25, menu == i ? colours[i] : INNER, colours[i]);
        text_center(x, 75, 9, 0.21f,
                    menu == i ? BLACK : colours[i], labels[i]);
    }
}

static void draw_deck_header(int menu, const Track *track, int track_index, bool playing, bool master_tempo,
                             int playhead, int duration, float tempo, int tempo_range, float nudge,
                             int quantize_division, int beat_jump, bool quantize_enabled,
                             bool a_hot_cue, bool auto_cue, int cue) {
    char value[64];
    box(0, 0, 400, 240, BLACK, BORDER);
    top_menu(menu);
    box(8, 34, 38, 30, BLACK, BORDER);
    text_center(8, 38, 36, 0.25f, TEXT, "DECK");
    text_center(8, 38, 46, 0.46f, TEXT, "1");
    text(56, 35, 0.47f, TEXT, track ? track->title : "");
    text(56, 53, 0.30f, MUTED, track ? track->composer : "");
    box(5, 70, 82, 82, INNER, BORDER);
    draw_artwork(5, 70);
    text(94, 69, 0.34f, TEXT, "TRACK");
    snprintf(value, sizeof(value), track ? "%02d" : "--", track_index + 1);
    text(100, 80, 0.42f, TEXT, value);
    if (a_hot_cue) {
        status_box(150, 74, 74, "", "", RED);
        text(153, 77, 0.31f, RED, "A.HOT.CUE");
    }
    if (auto_cue) {
        status_box(150, 96, 74, "", "", TEXT);
        text(153, 99, 0.31f, TEXT, "AUTO.CUE");
    }
    /* Keep the millisecond field on its own baseline.  The original compact
       placement overlapped the tall seconds glyphs on the 3DS bitmap font,
       even though the logical screen resolution is identical on 3DS/3DS LL. */
    snprintf(value, sizeof(value), "%02d:%02d", playhead / 60000, (playhead / 1000) % 60);
    /* Keep the seconds and milliseconds as two deliberately separated
       fields.  The Japanese bitmap face has a tall descender on ':' and the
       old positions let the fraction overlap it on hardware. */
    char milliseconds[16];
    snprintf(milliseconds, sizeof(milliseconds), ".%03d", playhead % 1000);
    text_time_center(196, 121, value, milliseconds);
    text(302, 66, 0.39f, MUTED, "TEMPO");
    snprintf(value, sizeof(value), "%+.2f%%", tempo);
    text_right(391, 89, 0.62f, TEXT, value);
    snprintf(value, sizeof(value), "\xC2\xB1%d", tempo_range);
    text_right(391, 76, 0.40f, tempo_range_colour(tempo_range), tempo_range == 100 ? "WIDE" : value);
    (void)nudge;
    {
        box(294, 118, 98, 40, BLACK, AMBER);
        text(300, 120, 0.39f, MUTED, "BPM");
        const float rate_factor = 1.0f + tempo / 100.0f;
        const u32 displayed_bpm = track && track->bpm_100 ?
            (u32)lroundf(track->bpm_100 * (rate_factor > 0.0f ? rate_factor : 0.0f)) : 0;
        char integer[16], decimal[8];
        if (displayed_bpm) {
            snprintf(integer, sizeof(integer), "%d", (int)(displayed_bpm / 100));
            snprintf(decimal, sizeof(decimal), ".%02d", (int)(displayed_bpm % 100));
        } else {
            snprintf(integer, sizeof(integer), "--");
            snprintf(decimal, sizeof(decimal), ".--");
        }
        text_right(362, 131, 0.72f, TEXT, integer);
        text(365, 140, 0.41f, TEXT, decimal);
    }
    /* Restored performance controls frame a deliberately narrower overview waveform. */
    if (quantize_enabled) {
        box(4, 162, 76, 29, BLACK, RED);
        text_center(4, 76, 164, 0.28f, RED, "QUANTIZE");
        text_center(4, 76, 177, 0.40f, TEXT,
                    quantize_division == 1 ? "1" : quantize_division == 2 ? "1/2" : "1/4");
    }
    box(4, 195, 76, 29, BLACK, TEXT);
    text_center(4, 76, 197, 0.29f, TEXT, "BEAT JUMP");
    snprintf(value, sizeof(value), "%d", beat_jump);
    text_center(4, 76, 210, 0.40f, TEXT, value);
    if (master_tempo) {
        /* Master Tempo and musical Key are independent indicators.  Their
           former borderless stack read as a single "MT KEY" control. */
        box(341, 162, 50, 24, BLACK, RED);
        text_center(341, 50, 165, 0.40f, RED, "MT");
    }
    /* Key is always meaningful even when Master Tempo is disabled.  It is a
       plain label/value, not a second MT-style button. */
    text_center(341, 50, 192, 0.35f, MUTED, "KEY");
    text_center(341, 50, 205, 0.43f, TEXT, track ? track->key : "");
    if (track) {
        /* Leave clear lanes around the performance controls: cue flags sit
           above the overview, so the complete wave cannot enter BPM/KEY. */
        waveform(86, 246, 179, 51, duration ? (float)playhead / duration : 0.0f, false, false,
                 track->preview_waveform, track->waveform_colours);
        draw_overview_cues(track, duration, 86, 246, 179, cue);
    }
}

static void draw_browser(int selected, bool playing) {
    const int rows = library_count < 3 ? library_count : 3;
    int first = selected - 1;
    if (first < 0) first = 0;
    if (first > library_count - rows) first = library_count - rows;
    box(0, 0, 400, 240, BLACK, BORDER);
    text(12, 14, 0.55f, TEXT, "BROWSER");
    text_right(390, 18, 0.28f, MUTED, "A LOAD   X CLOSE");
    text_right(390, 34, 0.24f, MUTED, "D-PAD SELECT");
    if (rows == 0) text(19, 84, 0.40f, MUTED, "NO MP3 OR M4A FOUND IN /Contents");
    /* During playback, refresh exactly one changed cover per video frame.
       All visible rows become current within three frames without a burst of
       SD reads competing with the audio queue. */
    if (playing && rows > 0) {
        const int slot = browser_artwork_refresh_slot++ % rows;
        const int track_index = first + slot;
        if (browser_artwork_track[slot] != track_index)
            load_browser_artwork(slot, track_index);
    }
    for (int i = 0; i < rows; ++i) {
        const int track_index = first + i;
        const float y = 50 + i * 63;
        box(0, y, 400, 63, track_index == selected ? INNER : BLACK,
            track_index == selected ? CYAN : BORDER);
        if (!playing) load_browser_artwork(i, track_index);
        draw_browser_artwork(i, 0, y, 63);
        text(72, y + 8, 0.43f, track_index == selected ? TEXT : MUTED,
             library_tracks[track_index].title);
        text(72, y + 33, 0.34f, MUTED, library_tracks[track_index].composer);
        text_right(390, y + 35, 0.30f, library_tracks[track_index].is_m4a ? GREEN : CYAN,
                   library_tracks[track_index].is_m4a ? "M4A / AAC" : "MP3");
    }
    char count[32];
    snprintf(count, sizeof(count), "%02d / %02d", selected + 1, library_count);
    text_right(390, 34, 0.28f, MUTED, count);
    /* A media file can exist in the exported library but still use a codec
       profile the 3DS decoder cannot open. Keep that failure in the browser
       instead of entering a non-playing Deck, where B/A would look broken. */
    const char *load_error = audio_status();
    if (load_error && strstr(load_error, "ERROR")) {
        box(8, 218, 384, 14, BLACK, RED);
        text_center(8, 384, 220, 0.30f, RED, load_error);
    }
}

static int memory_cue_count(const Track *track);

static void draw_runtime(int playhead, int duration, bool playing, bool vinyl, float nudge, int jog_delta,
                         const Track *track, int menu, int loop, float tempo, int tempo_range,
                         int quantize_division, int beat_jump, bool loop_active, bool a_hot_cue,
                         bool auto_cue, bool master_tempo, bool quantize_enabled, int cue,
                         int loop_start, int loop_end) {
    box(0, 0, 320, 240, BLACK, BORDER);
    /* Setting intentionally owns the entire touch screen; performance data is
       hidden until the user returns to a deck menu. */
    if (menu == 4) {
        text(12, 10, 0.46f, TEXT, "SETTING");
        text(12, 28, 0.25f, MUTED, "TAP - / + TO CHANGE VALUES");
        box(8, 43, 304, 31, INNER, RED);
        text(18, 50, 0.35f, RED, "QUANTIZE");
        box(120, 47, 53, 23, quantize_enabled ? RED : BLACK, RED);
        text_center(120, 53, 52, 0.29f, quantize_enabled ? BLACK : RED,
                    quantize_enabled ? "ON" : "OFF");
        box(181, 47, 33, 23, BLACK, RED); text_center(181, 33, 52, 0.34f, RED, "-");
        box(218, 47, 49, 23, RED, RED);
        text_center(218, 49, 52, 0.34f, BLACK,
                    quantize_division == 1 ? "1" : quantize_division == 2 ? "1/2" : "1/4");
        box(271, 47, 33, 23, BLACK, RED); text_center(271, 33, 52, 0.34f, RED, "+");
        box(8, 80, 304, 31, INNER, TEXT);
        text(18, 87, 0.35f, TEXT, "BEAT JUMP");
        box(184, 84, 42, 23, BLACK, TEXT); text_center(184, 42, 89, 0.34f, TEXT, "-");
        box(230, 84, 42, 23, TEXT, TEXT);
        char jump_value[8]; snprintf(jump_value, sizeof(jump_value), "%d", beat_jump);
        text_center(230, 42, 89, 0.34f, BLACK, jump_value);
        box(276, 84, 28, 23, BLACK, TEXT); text_center(276, 28, 89, 0.34f, TEXT, "+");
        box(8, 125, 304, 29, INNER, RED);
        text(18, 131, 0.35f, a_hot_cue ? RED : MUTED, "A.HOT.CUE");
        box(232, 128, 72, 23, a_hot_cue ? RED : BLACK, RED);
        text_center(232, 72, 133, 0.34f, a_hot_cue ? BLACK : RED, a_hot_cue ? "ON" : "OFF");
        box(8, 161, 304, 29, INNER, BORDER);
        text(18, 167, 0.35f, auto_cue ? BORDER : MUTED, "AUTO.CUE");
        box(232, 164, 72, 23, auto_cue ? BORDER : BLACK, BORDER);
        text_center(232, 72, 169, 0.34f, auto_cue ? BLACK : BORDER, auto_cue ? "ON" : "OFF");
        box(8, 197, 304, 29, INNER, RED);
        text(18, 203, 0.35f, RED, "MT (MASTER TEMPO)");
        box(232, 200, 72, 23, master_tempo ? RED : BLACK, RED);
        text_center(232, 72, 205, 0.34f, master_tempo ? BLACK : RED, master_tempo ? "ON" : "OFF");
        return;
    }
    /* Top strip: tempo fader and its fixed operating range. */
    text(9, 5, 0.31f, MUTED, "TEMPO");
    C2D_DrawRectSolid(9, 19, 0.0f, 240, 8, INNER);
    C2D_DrawLine(9, 23, BORDER, 249, 23, BORDER, 1.0f, 0.0f);
    /* The fixed centre tick remains visible when the fader is at its default. */
    C2D_DrawLine(129, 13, TEXT, 129, 17, TEXT, 1.0f, 0.0f);
    C2D_DrawLine(129, 29, TEXT, 129, 33, TEXT, 1.0f, 0.0f);
    const float fader_x = 129.0f + tempo * 120.0f / tempo_range;
    C2D_DrawLine(fader_x, 16, GREEN, fader_x, 30, GREEN, 3.0f, 0.0f);
    char tempo_value[24]; snprintf(tempo_value, sizeof(tempo_value), "%+.2f%%", tempo);
    text_right(249, 3, 0.42f, TEXT, tempo_value);
    const u32 range_colour = tempo_range_colour(tempo_range);
    box(260, 5, 53, 25, BLACK, range_colour);
    char range_value[16]; snprintf(range_value, sizeof(range_value), "\xC2\xB1%d", tempo_range);
    text_center(260, 53, 10, 0.40f, range_colour, tempo_range == 100 ? "WIDE" : range_value);
    const int waveform_y = 51, waveform_height = 97;
    const float visual_factor = tempo_visual_factor(tempo);
    draw_runtime_waveform(track, playhead, duration, waveform_y, waveform_height, visual_factor);
    if (loop_active && track) {
        const float loop_left = runtime_x_for_time((u32)loop_start, playhead, visual_factor);
        const float loop_right = runtime_x_for_time((u32)loop_end, playhead, visual_factor);
        const float left = loop_left < loop_right ? loop_left : loop_right;
        const float right = loop_left < loop_right ? loop_right : loop_left;
        if (right > 0.0f && left < 320.0f) {
            const float clipped_left = left < 0.0f ? 0.0f : left;
            const float clipped_right = right > 320.0f ? 320.0f : right;
            C2D_DrawRectSolid(clipped_left, waveform_y, 0.0f,
                              clipped_right - clipped_left, waveform_height,
                              (AMBER & 0x00ffffffu) | 0x30000000u);
            C2D_DrawLine(clipped_left, waveform_y, AMBER,
                         clipped_left, waveform_y + waveform_height, AMBER, 1.0f, 0.0f);
            C2D_DrawLine(clipped_right, waveform_y, AMBER,
                         clipped_right, waveform_y + waveform_height, AMBER, 1.0f, 0.0f);
        }
    }
    if (track && duration > 0) {
        for (int i = 0; i < MAX_BEAT_GRID && track->beat_times[i] != 0xffffffff; ++i) {
            const float x = runtime_x_for_time(track->beat_times[i], playhead, visual_factor);
            if (x >= 0.0f && x < 320.0f) {
                const u32 grid_colour = track->beat_numbers[i] == 1 ? RED : TEXT;
                C2D_DrawLine(x, waveform_y - 6, grid_colour, x, waveform_y - 2, grid_colour, 1.0f, 0.0f);
                C2D_DrawLine(x, waveform_y + waveform_height + 2, grid_colour,
                             x, waveform_y + waveform_height + 6, grid_colour, 1.0f, 0.0f);
            }
        }
        for (int i = 0; i < 16 && track->cue_times[i] != 0xffffffff; ++i) {
            const float x = runtime_x_for_time(track->cue_times[i], playhead, visual_factor);
            if (x < -7.0f || x > 327.0f || track->cue_numbers[i]) continue;
            const u32 colour = !track->cue_colours[i] || track->cue_colours[i] == AMBER ?
                RED : track->cue_colours[i];
            /* Memory cues are downward triangles above the waveform. */
            C2D_DrawTriangle(x - 5, 39, colour, x + 5, 39, colour, x, waveform_y - 2, colour, 0.0f);
        }
        /* Draw hot cues after memory cues: a coincident hot cue deliberately stays on top. */
        for (int i = 0; i < 16 && track->cue_times[i] != 0xffffffff; ++i) {
            if (!track->cue_numbers[i]) continue;
            const float x = runtime_x_for_time(track->cue_times[i], playhead, visual_factor);
            if (x < -7.0f || x > 327.0f) continue;
            const u32 colour = track->cue_colours[i] ? track->cue_colours[i] : GREEN;
            const char label[2] = { (char)('A' + ((track->cue_numbers[i] - 1) % 8)), '\0' };
            C2D_DrawRectSolid(x - 6, 38, 0.0f, 12, 12, colour);
            text(x - 3, 40, 0.24f, BLACK, label);
        }
        /* The deck Cue point is distinct from rekordbox memory cues.  It is
           shown below the live waveform as a yellow upward triangle. */
        const float cue_x = runtime_x_for_time((u32)cue, playhead, visual_factor);
        if (cue_x >= -7.0f && cue_x <= 327.0f)
            C2D_DrawTriangle(cue_x, waveform_y + waveform_height + 3, AMBER,
                             cue_x - 5, waveform_y + waveform_height + 11, AMBER,
                             cue_x + 5, waveform_y + waveform_height + 11, AMBER, 0.0f);
    }
    if (!track) text_center(0, 320, 93, 0.31f, MUTED, "NO TRACK LOADED");
    /* Keep normal playback uncluttered, but make a failed file/decoder load
       actionable on the emulator and the console instead of silently leaving
       Play disabled. */
    const char *audio_error = audio_status();
    if (audio_error && strstr(audio_error, "ERROR")) {
        box(8, 149, 304, 10, BLACK, RED);
        text_center(8, 304, 150, 0.24f, RED, audio_error);
    }
    /* The pad bank changes with the selected performance menu. */
    if (menu == 0) {
        for (int i = 0; i < 8; ++i) {
            const float x = 3 + (i % 4) * 79;
            const float y = 160 + (i / 4) * 38;
            u32 pad_colour = PAD_EMPTY;
            if (track) for (int cue_index = 0; cue_index < 16; ++cue_index)
                if (track->cue_numbers[cue_index] == i + 1) {
                    pad_colour = track->cue_colours[cue_index] ? track->cue_colours[cue_index] : GREEN;
                    break;
                }
            box(x, y, 74, 35, INNER, pad_colour);
            char pad[2] = { (char)('A' + i), '\0' };
            text_center(x, 74, y + 9, 0.44f, pad_colour, pad);
        }
    } else if (menu == 1) {
        const int count = memory_cue_count(track);
        text_center(0, 320, 163, 0.30f, MUTED, "D-PAD: PREV / NEXT MEMORY CUE");
        box(8, 183, 148, 46, INNER, AMBER);
        text_center(8, 148, 194, 0.42f, AMBER, "MEMORY");
        box(164, 183, 148, 46, INNER, RED);
        text_center(164, 148, 194, 0.42f, RED, "DELETE");
        char count_value[24]; snprintf(count_value, sizeof(count_value), "%d / 8", count);
        text_center(0, 320, 170, 0.28f, TEXT, count_value);
    } else if (menu == 2) {
        static const char *const labels[] = { "LOOP IN", "LOOP OUT", "4 BEATS", "8 BEATS", "EXIT" };
        static const char *const sublabels[] = { "CUE SET", "CUE > OUT", "x1/2", "x2", "" };
        for (int i = 0; i < 5; ++i) {
            const float x = 3 + i * 63;
            const bool active = loop_active && (i == 2 || i == 3);
            box(x, 166, 58, 55, active ? AMBER : INNER, AMBER);
            text_center(x, 58, 176, i < 2 ? 0.23f : 0.27f, active ? BLACK : AMBER, labels[i]);
            if (sublabels[i][0]) text_center(x, 58, 199, 0.25f, active ? BLACK : TEXT, sublabels[i]);
        }
    } else if (menu == 3) {
        static const char *const labels[] = {
            "\xE2\x97\x80 1", "\xE2\x97\x80 2", "\xE2\x97\x80 4", "\xE2\x97\x80 8",
            "1 \xE2\x96\xB6", "2 \xE2\x96\xB6", "4 \xE2\x96\xB6", "8 \xE2\x96\xB6"
        };
        for (int i = 0; i < 8; ++i) {
            const float x = 3 + (i % 4) * 79;
            const float y = 160 + (i / 4) * 38;
            box(x, y, 74, 35, INNER, BLUE);
            text_center(x, 74, y + 9, 0.42f, BLUE, labels[i]);
        }
    }
    (void)playing; (void)vinyl; (void)nudge; (void)jog_delta;
}

static int quantized_cue_time(const Track *track, int playhead, int division) {
    if (!track || division < 1) return playhead;
    int best_time = playhead, best_distance = 0x7fffffff;
    for (int i = 0; i + 1 < MAX_BEAT_GRID && track->beat_times[i + 1] != 0xffffffff; ++i) {
        const int start = (int)track->beat_times[i], end = (int)track->beat_times[i + 1];
        for (int step = 0; step < division; ++step) {
            const int candidate = start + (end - start) * step / division;
            const int distance = abs(candidate - playhead);
            if (distance < best_distance) { best_time = candidate; best_distance = distance; }
        }
    }
    return best_time;
}

static bool trigger_hot_cue(Track *track, int number, int *playhead, bool *playing,
                            int quantize_division, bool *loop_active,
                            int *loop_start, int *loop_end) {
    if (!track || number < 1 || number > 8) return false;
    for (int i = 0; i < 16 && track->cue_times[i] != 0xffffffff; ++i) {
        if (track->cue_numbers[i] != number) continue;
        /* Do not move the deck UI ahead of audio.  The cue becomes "playing"
           only after two fresh PCM buffers have been decoded and queued. */
        *playing = false;
        audio_set_paused(true);
        *playhead = (int)track->cue_times[i];
        if (audio_trigger_preloaded_hot_cue((u8)number) ||
            audio_seek_hot_cue_ms(track->cue_times[i])) {
            *playing = true;
            audio_set_paused(false);
            const u32 saved_loop_end = track->hotcue_loop_ends[number - 1];
            if (loop_active && loop_start && loop_end && saved_loop_end > track->cue_times[i] &&
                saved_loop_end != 0xffffffffu) {
                *loop_active = true;
                *loop_start = (int)track->cue_times[i];
                *loop_end = (int)saved_loop_end;
                /* A loop created at this cue retains its prepared PCM cache
                   until another loop replaces it; in that common case this
                   reinstates the loop without a decode on the pad hit. */
                audio_set_loop(track->cue_times[i], saved_loop_end, true);
            } else if (loop_active) {
                *loop_active = false;
                audio_set_loop(0, 0, false);
            }
        }
        return false;
    }
    /* An unused pad creates its hot cue at the current playback position. */
    for (int i = 0; i < 16; ++i) if (track->cue_times[i] == 0xffffffff) {
        const bool stores_loop = loop_active && *loop_active && loop_start && loop_end &&
            *loop_end > *loop_start;
        track->cue_times[i] = stores_loop ? (u32)*loop_start :
            (u32)quantized_cue_time(track, *playhead, quantize_division);
        track->cue_numbers[i] = (u8)number;
        track->cue_colours[i] = stores_loop ? AMBER : GREEN;
        track->hotcue_loop_ends[number - 1] = stores_loop ? (u32)*loop_end : 0xffffffffu;
        /* An assigned pad is prepared immediately.  Pad presses themselves
           never take the per-file decoder/SD-card path. */
        if (audio_cache_hot_cue((u8)number, track->cue_times[i], (u32)*playhead) && *playing)
            audio_set_paused(false);
        return true;
    }
    return false;
}

static bool delete_hot_cue(Track *track, int number) {
    if (!track || number < 1 || number > 8) return false;
    for (int i = 0; i < 16 && track->cue_times[i] != 0xffffffff; ++i) {
        if (track->cue_numbers[i] != number) continue;
        for (int j = i; j + 1 < 16; ++j) {
            track->cue_times[j] = track->cue_times[j + 1];
            track->cue_numbers[j] = track->cue_numbers[j + 1];
            track->cue_colours[j] = track->cue_colours[j + 1];
        }
        track->cue_times[15] = 0xffffffff;
        track->cue_numbers[15] = 0;
        track->cue_colours[15] = 0;
        track->hotcue_loop_ends[number - 1] = 0xffffffffu;
        return true;
    }
    return false;
}

static int memory_cue_count(const Track *track) {
    int count = 0;
    if (!track) return 0;
    for (int i = 0; i < 16 && track->cue_times[i] != 0xffffffff; ++i)
        if (!track->cue_numbers[i]) ++count;
    return count;
}

static bool add_memory_cue(Track *track, int playhead, int quantize_division, int *memory_cursor) {
    if (!track || memory_cue_count(track) >= 8) return false;
    for (int i = 0; i < 16; ++i) if (track->cue_times[i] == 0xffffffff) {
        track->cue_times[i] = (u32)quantized_cue_time(track, playhead, quantize_division);
        track->cue_numbers[i] = 0;
        track->cue_colours[i] = RED;
        if (memory_cursor) *memory_cursor = memory_cue_count(track) - 1;
        return true;
    }
    return false;
}

static bool delete_memory_cue(Track *track, int *memory_cursor) {
    if (!track || !memory_cursor) return false;
    const int slot = *memory_cursor;
    if (slot < 0) return false;
    int memory_index = 0;
    for (int i = 0; i < 16 && track->cue_times[i] != 0xffffffff; ++i) {
        if (track->cue_numbers[i]) continue;
        if (memory_index++ != slot) continue;
        /* Compact the fixed cache layout so all subsequent cue scans remain valid. */
        for (int j = i; j + 1 < 16; ++j) {
            track->cue_times[j] = track->cue_times[j + 1];
            track->cue_numbers[j] = track->cue_numbers[j + 1];
            track->cue_colours[j] = track->cue_colours[j + 1];
        }
        track->cue_times[15] = 0xffffffff;
        track->cue_numbers[15] = 0;
        track->cue_colours[15] = 0;
        const int remaining = memory_cue_count(track);
        if (*memory_cursor >= remaining) *memory_cursor = remaining - 1;
        return true;
    }
    return false;
}

static bool memory_cue_move(const Track *track, int playhead, int direction,
                            int *memory_cursor, int *target) {
    if (!track || !memory_cursor || !target || !direction) return false;
    int selected = -1;
    u32 selected_time = direction < 0 ? 0 : 0xffffffff;
    int memory_index = 0;
    for (int i = 0; i < 16 && track->cue_times[i] != 0xffffffff; ++i) {
        if (track->cue_numbers[i]) continue;
        const u32 time = track->cue_times[i];
        const bool before = direction < 0 && (int)time < playhead && (selected < 0 || time > selected_time);
        const bool after = direction > 0 && (int)time > playhead && (selected < 0 || time < selected_time);
        if (before || after) { selected = memory_index; selected_time = time; }
        ++memory_index;
    }
    if (selected < 0) return false;
    *memory_cursor = selected;
    *target = (int)selected_time;
    return true;
}

static bool beat_jump_move(const Track *track, int playhead, int beats, int direction, int *target) {
    if (!track || !target || beats < 1 || !direction) return false;
    int anchor = -1;
    for (int i = 0; i < MAX_BEAT_GRID && track->beat_times[i] != 0xffffffff; ++i) {
        if ((int)track->beat_times[i] > playhead) break;
        anchor = i;
    }
    if (anchor < 0) anchor = 0;
    const int destination = anchor + direction * beats;
    if (destination < 0 || destination >= MAX_BEAT_GRID || track->beat_times[destination] == 0xffffffff)
        return false;
    *target = (int)track->beat_times[destination];
    return true;
}

/* Both the blue Beat Jump pads and Y + D-pad arrive here.  The eight reserved
   PCM slots are keyed by distance and direction, leaving A–H Hot Cue caches
   untouched.  A previously prepared destination therefore starts by queueing
   PCM directly to NDSP instead of seeking/decoding on the button press. */
static bool trigger_beat_jump(const Track *track, int beats, int direction,
                              int *playhead, bool *playing) {
    int destination;
    if (!track || !playhead || !playing || !beat_jump_move(track, *playhead, beats, direction, &destination))
        return false;
    int distance_index = beats == 1 ? 0 : beats == 2 ? 1 : beats == 4 ? 2 : beats == 8 ? 3 : 4;
    const u8 cache_number = (u8)(9 + distance_index * 2 + (direction > 0 ? 1 : 0));
    const bool resume = *playing;
    const int previous = *playhead;
    *playing = false;
    audio_set_paused(true);
    *playhead = destination;
    /* Rebuild only when this direction/distance slot represents a different
       grid coordinate. Subsequent taps use the exact same no-I/O hot-cue
       hand-off and update the visible transport at the same instant. */
    bool ready = audio_cached_hot_cue_matches(cache_number, (u32)destination) &&
                 audio_trigger_preloaded_hot_cue(cache_number);
    if (!ready) {
        if (audio_cache_hot_cue(cache_number, (u32)destination, (u32)previous))
            ready = audio_trigger_preloaded_hot_cue(cache_number);
    }
    if (!ready) ready = audio_seek_hot_cue_ms((u32)destination);
    *playing = ready && resume;
    audio_set_paused(!*playing);
    return ready;
}

/* Fill the Beat Jump PCM slots while a track is loading, never as the first
   touch during a performance.  The source is restored after each cache so
   the deck begins exactly at Auto Cue (or zero). */
static void preload_beat_jump_targets(const Track *track, int playhead) {
    static const int distances[] = { 1, 2, 4, 8, 16 };
    /* AAC random access is more expensive. Keep its decoder state pristine
       while loading; declared Hot Cues still get PCM caches. */
    if (!track || track->is_m4a || !audio_is_loaded()) return;
    for (int index = 0; index < 5; ++index) {
        for (int direction_index = 0; direction_index < 2; ++direction_index) {
            const int direction = direction_index ? 1 : -1;
            int destination;
            if (!beat_jump_move(track, playhead, distances[index], direction, &destination)) continue;
            const u8 cache_number = (u8)(9 + index * 2 + direction_index);
            audio_cache_hot_cue(cache_number, (u32)destination, (u32)playhead);
        }
    }
}

static u32 track_auto_cue(const Track *track) {
    if (!track || !track->duration_ms) return 0;
    /* The device-library beat grid is the common source-time coordinate for
       cue, grid and waveform. A waveform-amplitude threshold is not an Auto
       Cue because it is a display value, not an analysed transport position. */
    if (track->beat_times[0] != 0xffffffff)
        return track->beat_times[0];
    return 0;
}

static bool activate_loop(const Track *track, int ticks, bool quantize, int playhead,
                          int *loop_start, int *loop_end) {
    if (!track || ticks <= 0 || !track->duration_ms) return false;
    int beat_index = -1, best_distance = 0x7fffffff;
    for (int i = 0; i < MAX_BEAT_GRID && track->beat_times[i] != 0xffffffff; ++i) {
        const int distance = abs((int)track->beat_times[i] - playhead);
        /* Ties choose the later beat: it prevents an intended immediate
           loop from unexpectedly landing one beat behind the finger. */
        if (distance <= best_distance) { beat_index = i; best_distance = distance; }
        if ((int)track->beat_times[i] > playhead && distance > best_distance) break;
    }
    if (beat_index < 0) beat_index = 0;
    const int start = quantize ? (int)track->beat_times[beat_index] : playhead;
    int end_index = beat_index + ticks / 8;
    if (quantize && ticks % 8 == 0 && end_index < MAX_BEAT_GRID && track->beat_times[end_index] != 0xffffffff) {
        *loop_end = (int)track->beat_times[end_index];
    } else {
        int interval = 0;
        if (beat_index + 1 < MAX_BEAT_GRID && track->beat_times[beat_index + 1] != 0xffffffff)
            interval = (int)track->beat_times[beat_index + 1] - (int)track->beat_times[beat_index];
        if (interval <= 0) interval = track->bpm_100 ? (int)(6000000u / track->bpm_100) : 500;
        *loop_end = start + interval * ticks / 8;
    }
    if (*loop_end <= start || *loop_end > (int)track->duration_ms) return false;
    *loop_start = start;
    return true;
}

static void show_startup_logo(C3D_RenderTarget *top, C3D_RenderTarget *bottom) {
    C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
    C2D_TargetClear(top, BLACK); C2D_SceneBegin(top); C2D_TextBufClear(text_buf);
    if (startup_logo_texture_ready)
        C2D_DrawImageAt(startup_logo_image, 59, 51, 0.0f, NULL, 1.10f, 1.10f);
    else
        text_center(0, 400, 90, 0.76f, TEXT, "PIONEER DJ");
    C2D_TargetClear(bottom, BLACK); C2D_SceneBegin(bottom); C2D_TextBufClear(text_buf);
    if (rekordbox_logo_texture_ready)
        C2D_DrawImageAt(rekordbox_logo_image, 32, 56, 0.0f, NULL, 1.00f, 1.00f);
    else
        text_center(0, 320, 106, 0.58f, TEXT, "rekordbox");
    C3D_FrameEnd(0);
    svcSleepThread(1000 * 1000 * 1000LL);
}

int main(void) {
    romfsInit();
    /* C2D_FontLoadSystem requires CFGU to be active.  Without this the
       Japanese font handle is NULL and every non-Latin tag becomes '?'. */
    cfguInit();
    gfxInitDefault(); C3D_Init(C3D_DEFAULT_CMDBUF_SIZE); C2D_Init(C2D_DEFAULT_MAX_OBJECTS); C2D_Prepare();
    audio_init();
    C3D_RenderTarget *top = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
    C3D_RenderTarget *bottom = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);
    text_buf = C2D_TextBufNew(16384);
    /* Match the Japanese 3DS HOME/menu font. It covers all standard Japanese
       characters without depending on the old per-library glyph subset. */
    pixel_font = C2D_FontLoadSystem(CFG_REGION_JPN);
    /* Retain the generated Noto subset solely as a recovery fallback for a
       damaged system-font handle; normal rendering never mixes font families. */
    japanese_font = C2D_FontLoad("sdmc:/3ds/3ds_one_deck/cache/NotoSansJP.bcfnt");
    load_startup_logos();
    show_startup_logo(top, bottom);
    View view = VIEW_DECK;
    /* Load every exported track without a manual refresh.  Cached rekordbox
       entries retain their analysis data; newly exported ASCII-path audio is
       appended immediately and can be loaded from Browser on this launch. */
    load_rekordbox_cache();
    scan_library("sdmc:/Contents", 0);
    load_cue_overrides();
    if (library_count == 0) {
        add_preview_track("10SAI - 04 SI", "10SAI", false);
        add_preview_track("Sunny After Rain", "aran", true);
        add_preview_track("Music is The Answer", "C-Show", true);
    }
    int menu = 0, browser = 0, loaded_track = -1, loop = 3, cue = 0, playhead = 0;
    int memory_cursor = -1, last_touch_x = 160, jog_delta = 0;
    int scrub_start_playhead = 0, scrub_start_x = 160;
    int tempo_range_index = 0;
    int duration = 248000;
    bool playing = false, master_tempo = true, auto_cue = true, a_hot_cue = true;
    bool touch_active = false, touch_ui = false, vinyl = false, cue_held = false;
    /* Quantize is a beat subdivision, not a binary mode: 1, 1/2 or 1/4. */
    int quantize_division = 1;
    bool quantize_enabled = true, loop_active = false;
    int beat_jump = 16, loop_start = 0, loop_end = 0, loop_ticks = 32;
    bool scrub_restore_playing = false, scrub_pending = false, scrub_audio_ready = false;
    u64 scrub_preview_tick = 0;
    u64 scrub_motion_tick = 0;
    u64 playback_anchor_tick = osGetTime();
    int playback_anchor_ms = 0;
    float nudge = 0.0f, tempo = 0.0f, slide_pitch_bend = 0.0f;
    /* MT starts enabled, matching the deck UI. At +/-0 it takes the direct
       PCM path, so this does not invoke time-stretch processing. */
    audio_set_master_tempo(master_tempo);
    while (aptMainLoop()) {
        hidScanInput();
        /* Service NDSP before any touch/UI work.  A Loop Out may land within
           this frame, and presenting a waveform is always less important
           than keeping the next PCM block queued. */
        audio_update();
        const u32 down = hidKeysDown(), held = hidKeysHeld(), up = hidKeysUp();
        const u64 now = osGetTime();
        /* Circle-pad input is deliberately read only as an analogue pitch
           bend.  All UI tests below use KEY_D*, rather than KEY_LEFT/RIGHT
           aliases (which include KEY_CPAD_* in libctru). */
        circlePosition slide_pad;
        hidCircleRead(&slide_pad);
        float requested_bend = 0.0f;
        if (view == VIEW_DECK && playing && !(touch_active && !touch_ui)) {
            const float x = (float)slide_pad.dx / 156.0f;
            if (fabsf(x) > 0.14f)
                requested_bend = (x > 0.0f ? x - 0.14f : x + 0.14f) / 0.86f * 4.0f;
        }
        if (fabsf(requested_bend - slide_pitch_bend) >= 0.02f) {
            slide_pitch_bend = requested_bend;
            audio_set_pitch_bend(slide_pitch_bend);
        }
        /* Service NDSP before any transport/UI work.  Audio continuity takes
           priority over waveform drawing, browser interaction and textures. */
        audio_update();
        if (playing && !(touch_active && vinyl && !touch_ui)) {
            u32 dsp_playhead = 0;
            if (audio_get_transport_ms(&dsp_playhead)) {
                playhead = (int)dsp_playhead;
                playback_anchor_ms = playhead;
                playback_anchor_tick = now;
            } else {
                playhead = timeline_position(playback_anchor_ms, playback_anchor_tick, tempo, duration, now);
            }
            if (loop_active && loop_end > loop_start && playhead >= loop_end) {
                /* Audio.c owns the NDSP hand-off; never seek here.  The old
                   UI held the marker at Loop Out until NDSP reported its
                   direct PCM buffer as PLAYING, creating a visible hitch at
                   every wrap.  Keep the visual transport in the same loop
                   timebase while that one-frame status transition completes. */
                const int loop_duration = loop_end - loop_start;
                playhead = loop_start + (playhead - loop_end) % loop_duration;
                playback_anchor_ms = playhead;
                playback_anchor_tick = now;
            } else if (playhead >= duration) {
                playing = false;
                audio_set_paused(true);
                playback_anchor_ms = playhead;
                playback_anchor_tick = now;
            }
        }
        if (down & KEY_START) break;
        if (down & KEY_B) {
            if (audio_is_loaded()) {
                playback_anchor_ms = playhead;
                playback_anchor_tick = now;
                playing = !playing;
                audio_set_paused(!playing);
            }
        }
        if ((down & KEY_A) && view == VIEW_DECK) {
            if (playing) {
                /* CDJ Cue while playing: cut sound first, then return to the
                   Cue coordinate silently.  It must not audition a fragment
                   before returning. */
                cue_held = false;
                playing = false;
                audio_set_paused(true);
                playhead = cue;
                if (audio_is_loaded()) audio_seek_hot_cue_ms((u32)cue);
            } else {
                /* From pause, establish Cue and play only while A is held. */
                cue = playhead;
                cue_held = true;
                audio_set_paused(true);
                if (audio_is_loaded() && audio_seek_hot_cue_ms((u32)cue)) {
                    playing = true;
                    audio_set_paused(false);
                }
            }
            playback_anchor_ms = playhead;
            playback_anchor_tick = osGetTime();
        }
        if ((up & KEY_A) && view == VIEW_DECK && cue_held) {
            /* Releasing A always returns the transport and audio decoder to
               the exact Cue coordinate, ready for the next cue audition. */
            cue_held = false;
            playing = false;
            audio_set_paused(true);
            playhead = cue;
            if (audio_is_loaded()) audio_seek_ms((u32)cue);
            playback_anchor_ms = playhead;
            playback_anchor_tick = osGetTime();
        }
        if (view == VIEW_BROWSER) {
            if (library_count > 0 && (down & KEY_DUP)) browser = (browser + library_count - 1) % library_count;
            if (library_count > 0 && (down & KEY_DDOWN)) browser = (browser + 1) % library_count;
            if (down & KEY_X) {
                view = VIEW_DECK;
            } else if (library_count > 0 && (down & KEY_A)) {
                const Track *candidate = &library_tracks[browser];
                const int candidate_duration = candidate->duration_ms ? (int)candidate->duration_ms : 248000;
                audio_set_duration_ms((u32)candidate_duration);
                audio_set_gapless_delay_samples(candidate->audio_delay_samples);
                audio_set_mp3_sample_rate(candidate->mp3_sample_rate);
                audio_set_mp3_seek_index(candidate->mp3_seek_offsets,
                                         candidate->mp3_seek_skips, MP3_SEEK_POINTS);
                const bool opened = candidate->is_m4a ? audio_load_m4a(candidate->path)
                                                       : audio_load_mp3(candidate->path);
                /* Keep the working deck and browser view intact on failure.
                   The browser renders the exact decoder error for this file. */
                if (!opened) continue;
                loaded_track = browser;
                loop_active = false;
                duration = candidate_duration;
                playing = false;
                playhead = auto_cue ? (int)track_auto_cue(candidate) : 0;
                cue = playhead;
                memory_cursor = -1;
                if (playhead > 0) audio_seek_ms((u32)playhead);
                /* Pre-expand exported MP3 Hot Cues at load time.  Pad hits
                   never perform filesystem I/O or a frame decode. */
                audio_preload_hot_cues(candidate->cue_times, candidate->cue_numbers,
                                       16, candidate->bpm_100, (u32)playhead);
                preload_beat_jump_targets(candidate, playhead);
                playback_anchor_ms = playhead;
                playback_anchor_tick = osGetTime();
                load_artwork(candidate);
                view = VIEW_DECK;
            }
        } else {
            if (!(held & (KEY_Y | KEY_R)) && (down & KEY_DUP) && runtime_wave_zoom_index < 4)
                ++runtime_wave_zoom_index;
            if (!(held & (KEY_Y | KEY_R)) && (down & KEY_DDOWN) && runtime_wave_zoom_index > 0)
                --runtime_wave_zoom_index;
            const int direction = (down & KEY_DLEFT) ? -1 : (down & KEY_DRIGHT) ? 1 : 0;
            if (direction && loaded_track >= 0 && (held & KEY_Y)) {
                if (trigger_beat_jump(&library_tracks[loaded_track], beat_jump, direction, &playhead, &playing)) {
                    playback_anchor_ms = playhead;
                    playback_anchor_tick = osGetTime();
                }
            } else if (direction && loaded_track >= 0 && !playing && (held & KEY_R)) {
                int destination;
                if (memory_cue_move(&library_tracks[loaded_track], playhead, direction,
                                    &memory_cursor, &destination)) {
                    playhead = destination;
                    audio_set_paused(true);
                    audio_seek_ms((u32)playhead);
                    playback_anchor_ms = playhead;
                    playback_anchor_tick = osGetTime();
                }
            } else if (direction < 0) {
                menu = (menu + 4) % 5;
            } else if (direction > 0) {
                menu = (menu + 1) % 5;
            }
            if (down & KEY_X) view = VIEW_BROWSER;
        }
        if (held & KEY_TOUCH) {
            touchPosition touch; hidTouchRead(&touch);
            if (!touch_active) {
                touch_active = true; touch_ui = false; last_touch_x = touch.px;
                vinyl = false; /* Y is reserved for Y + D-pad Beat Jump. */
                if (touch.py < 33 || touch.py >= 160 || menu == 4) {
                    touch_ui = true;
                    if (touch.py < 33 && touch.px >= 260) {
                        tempo_range_index = (tempo_range_index + 1) % 4;
                        const float limit = (float)tempo_ranges[tempo_range_index];
                        if (tempo > limit) tempo = limit;
                        if (tempo < -limit) tempo = -limit;
                        playback_anchor_ms = playhead;
                        playback_anchor_tick = now;
                        audio_set_tempo(tempo);
                    } else if (touch.py >= 160 && menu == 2 && touch.px >= 3 && touch.px < 318) {
                        const int selected = (touch.px - 3) / 63;
                        if (selected == 4) {
                            loop_active = false;
                            audio_set_loop(0, 0, false);
                        }
                        else if (loaded_track >= 0) {
                            Track *track = &library_tracks[loaded_track];
                            if (selected == 0) {
                                /* LOOP IN is the deck Cue point by definition. */
                                cue = quantize_enabled ?
                                    quantized_cue_time(track, playhead, quantize_division) : playhead;
                                loop_start = cue;
                                /* Prepare the Loop-In PCM prefix now, while
                                   the deck keeps playing. Loop Out can reuse
                                   it without a decoder seek on the pad hit. */
                                audio_prime_loop_in((u32)loop_start);
                            } else if (selected == 1) {
                                const int loop_out = quantize_enabled ?
                                    quantized_cue_time(track, playhead, quantize_division) : playhead;
                                if (cue < loop_out) {
                                    loop_start = cue;
                                    loop_end = loop_out;
                                    loop_active = true;
                                    const bool resume = playing;
                                    if (audio_arm_loop_capture((u32)loop_start, (u32)loop_end)) {
                                        audio_set_loop((u32)loop_start, (u32)loop_end, true);
                                        if (resume) audio_set_paused(false);
                                    } else if (audio_prepare_loop_cache((u32)loop_start, (u32)loop_end, (u32)playhead)) {
                                        /* Cache history was unavailable (for
                                           example, an old Loop In). Preserve
                                           the deterministic seek fallback. */
                                        audio_set_loop((u32)loop_start, (u32)loop_end, true);
                                        playhead = loop_start;
                                        playback_anchor_ms = playhead;
                                        playback_anchor_tick = osGetTime();
                                        if (resume) audio_set_paused(false);
                                    } else loop_active = false;
                                }
                            } else {
                                bool new_loop = false;
                                if (loop_active) {
                                    if (selected == 2 && loop_ticks > 2) loop_ticks /= 2;
                                    if (selected == 3 && loop_ticks < 512) loop_ticks *= 2;
                                    loop_active = activate_loop(track, loop_ticks, true, loop_start,
                                                                &loop_start, &loop_end);
                                    if (loop_active) {
                                        /* x2 never seeks: the audible and
                                           visual source position stays where
                                           it was. x1/2 seeks only when the
                                           shortened interval excludes it. */
                                        const bool outside_new_loop =
                                            playhead < loop_start || playhead >= loop_end;
                                        audio_resize_loop((u32)loop_start, (u32)loop_end);
                                        if (outside_new_loop) {
                                            bool resume = playing;
                                            playhead = loop_start;
                                            audio_set_paused(true);
                                            if (!audio_seek_hot_cue_ms((u32)loop_start)) resume = false;
                                            playing = resume;
                                            audio_set_paused(!playing);
                                            playback_anchor_ms = playhead;
                                            playback_anchor_tick = osGetTime();
                                        }
                                    }
                                } else {
                                    loop_ticks = selected == 2 ? 32 : 64;
                                    loop_active = activate_loop(track, loop_ticks, true, playhead,
                                                                &loop_start, &loop_end);
                                    if (loop_active) { cue = loop_start; new_loop = true; }
                                }
                                if (loop_active && new_loop) {
                                    const bool resume = playing;
                                    if (audio_arm_loop_capture((u32)loop_start, (u32)loop_end)) {
                                        audio_set_loop((u32)loop_start, (u32)loop_end, true);
                                        if (resume) audio_set_paused(false);
                                    } else if (audio_prepare_loop_cache((u32)loop_start, (u32)loop_end, (u32)playhead)) {
                                        audio_set_loop((u32)loop_start, (u32)loop_end, true);
                                        playhead = loop_start;
                                        playback_anchor_ms = playhead;
                                        playback_anchor_tick = osGetTime();
                                        if (resume) audio_set_paused(false);
                                    } else loop_active = false;
                                }
                            }
                        }
                    } else if (touch.py >= 160 && menu == 3 && loaded_track >= 0) {
                        const int column = touch.px / 79;
                        const int row = touch.py >= 198 ? 1 : 0;
                        if (column >= 0 && column < 4 &&
                            trigger_beat_jump(&library_tracks[loaded_track], 1 << column,
                                              row ? 1 : -1, &playhead, &playing)) {
                            playback_anchor_ms = playhead;
                            playback_anchor_tick = osGetTime();
                        }
                    } else if (menu == 4 && touch.py >= 43 && touch.py < 74) {
                        if (touch.px >= 120 && touch.px < 173)
                            quantize_enabled = !quantize_enabled;
                        else if (touch.px >= 181 && touch.px < 218 && quantize_division < 4)
                            quantize_division *= 2;
                        else if (touch.px >= 276 && quantize_division > 1)
                            quantize_division /= 2;
                    } else if (menu == 4 && touch.py >= 80 && touch.py < 111) {
                        static const int beat_jump_values[] = { 1, 2, 4, 8, 16 };
                        int current = 0;
                        for (int i = 0; i < 5; ++i) if (beat_jump_values[i] == beat_jump) current = i;
                        if (touch.px >= 184 && touch.px < 230 && current > 0) --current;
                        else if (touch.px >= 276 && current < 4) ++current;
                        beat_jump = beat_jump_values[current];
                    } else if (menu == 4 && touch.py >= 125 && touch.py < 154 && touch.px >= 232) {
                        a_hot_cue = !a_hot_cue;
                    } else if (menu == 4 && touch.py >= 161 && touch.py < 190 && touch.px >= 232) {
                        auto_cue = !auto_cue;
                    } else if (menu == 4 && touch.py >= 197 && touch.py < 226 && touch.px >= 232) {
                        const bool desired = !master_tempo;
                        if (audio_set_master_tempo(desired)) master_tempo = desired;
                    } else if (touch.py >= 160 && menu == 0 && loaded_track >= 0) {
                        const int column = touch.px / 79;
                        const int row = touch.py >= 198 ? 1 : 0;
                        if (column >= 0 && column < 4) {
                            const int pad = row * 4 + column + 1;
                            Track *track = &library_tracks[loaded_track];
                            const bool changed = (held & KEY_Y) ?
                                delete_hot_cue(track, pad) :
                                trigger_hot_cue(track, pad, &playhead, &playing,
                                                quantize_enabled ? quantize_division : 0,
                                                &loop_active, &loop_start, &loop_end);
                            if (changed) {
                                mark_cue_override(track);
                                save_cue_overrides();
                            }
                        }
                        playback_anchor_ms = playhead;
                        /* audio_seek_ms is deliberately synchronous: anchor
                           the wave only after its fresh PCM has been queued
                           and the cue audio was unpaused. */
                        playback_anchor_tick = osGetTime();
                    } else if (touch.py >= 160 && menu == 1 && loaded_track >= 0) {
                        Track *track = &library_tracks[loaded_track];
                        const bool changed = touch.px < 160 ?
                            add_memory_cue(track, playhead,
                                           quantize_enabled ? quantize_division : 0, &memory_cursor) :
                            delete_memory_cue(track, &memory_cursor);
                        if (changed) {
                            mark_cue_override(track);
                            save_cue_overrides();
                        }
                        playback_anchor_ms = playhead;
                        /* audio_seek_ms is deliberately synchronous: anchor
                           the wave only after its fresh PCM has been queued
                           and the cue audio was unpaused. */
                        playback_anchor_tick = osGetTime();
                    }
                }
                if (!touch_ui && audio_is_loaded()) {
                    /* A waveform drag is a source-position scrub.  Pause now,
                       move the visual freely, then perform one real decoder
                       seek on release rather than decoding for every pixel. */
                    scrub_restore_playing = playing;
                    scrub_pending = false;
                    playing = false;
                    audio_set_paused(true);
                    playback_anchor_ms = playhead;
                    playback_anchor_tick = now;
                    scrub_start_playhead = playhead;
                    scrub_start_x = touch.px;
                    scrub_preview_tick = now - 100;
                    scrub_motion_tick = now;
                    scrub_audio_ready = audio_begin_scrub((u32)playhead);
                }
            }
            if (touch_ui && touch.py < 33 && touch.px >= 9 && touch.px <= 249) {
                const float limit = (float)tempo_ranges[tempo_range_index];
                tempo = (touch.px - 129.0f) * limit / 120.0f;
                if (tempo > limit) tempo = limit;
                if (tempo < -limit) tempo = -limit;
                playback_anchor_ms = playhead;
                playback_anchor_tick = now;
                audio_set_tempo(tempo);
            }
            const int moved = touch.px - last_touch_x;
            if (!touch_ui && moved != 0) {
                /* A drag uses the exact same time-to-pixel transform as the
                   runtime wave, so its release lands on the displayed sample
                   instead of an arbitrary milliseconds-per-pixel estimate. */
                const float factor = tempo_visual_factor(tempo);
                /* Match the 2deck jog direction: dragging right pulls the
                   source backwards; dragging left advances it.  This uses the
                   same pixels-per-millisecond value as the rendered wave. */
                playhead = scrub_start_playhead - (int)lroundf((touch.px - scrub_start_x) /
                    runtime_pixels_per_ms(factor));
                jog_delta = moved;
                if (!vinyl) nudge = moved * 0.40f;
                if (playhead < 0) playhead = 0;
                if (playhead > duration) playhead = duration;
                playback_anchor_ms = playhead;
                playback_anchor_tick = now;
                scrub_pending = true;
                /* Like the two-deck scratch reader, the signed touch delta
                   controls the audible direction.  The decoders are
                   forward-only, so audio_scrub_ms reverses the short PCM
                   grain for a backward drag before NDSP receives it. */
                const u64 elapsed = now > scrub_motion_tick ? now - scrub_motion_tick : 1;
                const float source_delta_ms = fabsf((float)moved / runtime_pixels_per_ms(factor));
                const float scrub_speed = source_delta_ms * 1000.0f / (float)elapsed;
                /* The decoder was expanded into a rolling PCM window at touch
                   start, so moving the platter never blocks the UI on a fresh
                   codec seek. The scratch source reads it continuously. */
                if (scrub_audio_ready &&
                    audio_set_scrub_position((u32)playhead, moved > 0, scrub_speed))
                    audio_set_paused(false);
                scrub_motion_tick = now;
                last_touch_x = touch.px;
            } else if (!touch_ui) {
                /* Holding the waveform still must be silent; do not keep
                   replaying the velocity from the previous touch sample. */
                if (scrub_audio_ready) audio_hold_scrub();
            }
        } else {
            if (up & KEY_TOUCH && touch_active && !touch_ui && audio_is_loaded()) {
                bool resume = scrub_restore_playing;
                audio_end_scrub();
                if (scrub_pending && !audio_seek_ms((u32)playhead)) resume = false;
                playing = resume;
                audio_set_paused(!playing);
                playback_anchor_ms = playhead;
                playback_anchor_tick = osGetTime();
            }
            if (up & KEY_TOUCH) {
                touch_active = false; touch_ui = false; vinyl = false;
                scrub_pending = false; scrub_audio_ready = false;
            }
            jog_delta = 0; nudge *= 0.82f;
            if (fabsf(nudge) < 0.01f) nudge = 0.0f;
        }
        /* Keep NDSP queue maintenance ahead of all UI work. */
        audio_update();
        C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
        C2D_TargetClear(top, BLACK); C2D_SceneBegin(top); C2D_TextBufClear(text_buf);
        if (view == VIEW_BROWSER) draw_browser(browser, playing); else draw_deck_header(menu, loaded_track >= 0 ? &library_tracks[loaded_track] : NULL, loaded_track, playing, master_tempo, playhead, duration, tempo, tempo_ranges[tempo_range_index], nudge, quantize_division, beat_jump, quantize_enabled, a_hot_cue, auto_cue, cue);
        C2D_TargetClear(bottom, BLACK); C2D_SceneBegin(bottom); C2D_TextBufClear(text_buf);
        draw_runtime(playhead, duration, playing, vinyl, nudge, jog_delta,
                     loaded_track >= 0 ? &library_tracks[loaded_track] : NULL, menu, loop, tempo, tempo_ranges[tempo_range_index], quantize_division, beat_jump, loop_active, a_hot_cue, auto_cue, master_tempo, quantize_enabled, cue, loop_start, loop_end);
        C3D_FrameEnd(0);
    }
    audio_exit();
    if (artwork_texture_ready) C3D_TexDelete(&artwork_texture);
    if (startup_logo_texture_ready) C3D_TexDelete(&startup_logo_texture);
    if (rekordbox_logo_texture_ready) C3D_TexDelete(&rekordbox_logo_texture);
    for (int i = 0; i < 3; ++i)
        if (browser_artwork_texture_ready[i]) C3D_TexDelete(&browser_artwork_textures[i]);
    if (pixel_font) C2D_FontFree(pixel_font);
    if (japanese_font) C2D_FontFree(japanese_font);
    C2D_TextBufDelete(text_buf); C2D_Fini(); C3D_Fini(); gfxExit(); cfguExit(); romfsExit(); return 0;
}
