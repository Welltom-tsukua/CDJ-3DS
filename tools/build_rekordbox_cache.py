"""Create the compact, read-only rekordbox cache used by the 3DS player.

The cache is deliberately generated next to the .3dsx on the exported SD card.
It contains only player-facing data: PDB metadata, preview and 2048-point USBANLZ
waveforms, cue colours, and 128px RGB565 cover art.  The 3DS reads this directly and does not
modify rekordbox's PIONEER database or analysis files.
"""

from __future__ import annotations

import argparse
import bisect
import difflib
import os
import re
import shutil
import struct
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
sys.path[:0] = [str(ROOT / "pydeps"), str(ROOT / "python-prodj-link")]

from PIL import Image
from prodj.pdblib.pdbdatabase import PDBDatabase
from prodj.pdblib.usbanlz import AnlzFile
from prodj.pdblib.usbanlzdatabase import UsbAnlzDatabase


MAGIC = b"RB3D15\0\0"
RUNTIME_WAVE_POINTS = 8192
REKORDBOX_BIG_WAVE_HZ = 150
RUNTIME_WAVE_HZ = 40
MAX_BEAT_GRID = 4096
MAX_TRACKS = 128
MP3_SEEK_POINTS = 16384
MP3_SEEK_STEP_MS = 50
RECORD = struct.Struct("<512s160s160s120s16s64sIIIIII400s400H8192H4096I4096B16I16B16I8I16384I16384H")


def fixed(text: object, size: int) -> bytes:
    raw = str(text or "").encode("utf-8", "replace")[: size - 1]
    return raw + b"\0" * (size - len(raw))


def looks_mojibake(value: object) -> bool:
    """PDB tags from older exports can be decoded as Latin-1 or replacement
    characters, while the actual exported FAT filename remains Unicode."""
    text = str(value or "")
    return "\ufffd" in text or any(0x80 <= ord(char) <= 0xFF for char in text)


def build_japanese_font(strings: list[str], output_root: Path) -> None:
    """Build only the glyphs this exported device library needs.

    The 3DS shared font is not reliable for Japanese under homebrew.  A full
    CJK font is unnecessarily large, so make a small BCFNT subset on the PC
    and let the 3DS load it from the cache directory.
    """
    glyphs = {code for value in strings for code in map(ord, value) if 0x20 <= code <= 0xFFFF}
    glyphs.update(range(0x20, 0x7F))
    # UI symbols and Japanese punctuation are not guaranteed to appear in the
    # current library, but must never render as a missing glyph after a later
    # export.  In particular, keep the common title brackets 【】 available.
    glyphs.update(range(0x3000, 0x3040))
    glyphs.update({
        0x00B1,  # plus/minus sign used by the tempo range display
        0x25B6, 0x25C0,  # Beat Jump arrows
        0x2190, 0x2192, 0x2212,
    })
    windows = Path(os.environ.get("WINDIR", "C:/Windows"))
    # Yu Gothic Bold has the same broad Japanese coverage as the Noto subset
    # but keeps legible strokes at the 3DS's low pixel density.
    font_source = windows / "Fonts" / "YuGothB.ttc"
    if not font_source.is_file():
        font_source = windows / "Fonts" / "NotoSansJP-VF.ttf"
    devkitpro = Path(os.environ.get("DEVKITPRO", "C:/devkitPro"))
    font_tool = devkitpro / "tools" / "bin" / "mkbcfnt.exe"
    if not font_tool.is_file():
        found = shutil.which("mkbcfnt")
        font_tool = Path(found) if found else font_tool
    if not font_source.is_file() or not font_tool.is_file():
        print("WARNING: Noto Sans JP or mkbcfnt not found; keeping the 3DS system-font fallback")
        return
    glyph_list = output_root / "NotoSansJP-codepoints.txt"
    glyph_list.write_text("\n".join(f"0x{code:04X}" for code in sorted(glyphs)), encoding="ascii")
    try:
        subprocess.run([str(font_tool), "-s", "22", "-w", str(glyph_list), "-o",
                        str(output_root / "NotoSansJP.bcfnt"), str(font_source)], check=True)
    except (OSError, subprocess.CalledProcessError) as error:
        print(f"WARNING: Japanese font generation failed: {error}")


def player_audio_path(root: Path, output_root: Path, identifier: str, source: Path) -> str:
    """Return a path libfat can open on the 3DS.

    The AAC decoder itself accepts the Japanese-name files, but the 3DS FAT
    path layer does not reliably open non-ASCII long filenames.  Keep ASCII
    paths in place and make a private ASCII alias only where it is necessary.
    """
    relative = source.relative_to(root).as_posix()
    if relative.isascii():
        return f"sdmc:/{relative}"
    audio_root = output_root / "audio"
    audio_root.mkdir(parents=True, exist_ok=True)
    destination = audio_root / f"{identifier}{source.suffix.lower()}"
    if not destination.is_file() or destination.stat().st_size != source.stat().st_size:
        try:
            # On the PC / Azahar setup both locations are normally NTFS.  A
            # hard link keeps the emulator library zero-copy while preserving
            # the exact ASCII alias needed by libfat.  FAT/exFAT SD exports do
            # not support links, so retain the ordinary copy fallback there.
            if destination.exists():
                destination.unlink()
            os.link(source, destination)
        except OSError:
            shutil.copyfile(source, destination)
    return f"sdmc:/3ds/3ds_one_deck/cache/audio/{destination.name}"


def transcode_aac_for_player(source: Path, output_root: Path, identifier: str, ffmpeg: Path,
                             bitrate: str) -> Path:
    """Create a deterministic MP3 performance copy for an AAC/M4A export.

    The original device-library file is never touched.  The player copy is
    deliberately ASCII-named, 44.1 kHz stereo CBR MP3 so the 3DS can use the
    indexed MP3 path for Hot Cues and Loop PCM without an AAC decoder seek at
    a performance boundary.
    """
    destination = output_root / "audio" / "performance" / f"{identifier}.mp3"
    destination.parent.mkdir(parents=True, exist_ok=True)
    try:
        fresh = (destination.is_file() and
                 destination.stat().st_size > 0 and
                 destination.stat().st_mtime_ns >= source.stat().st_mtime_ns)
    except OSError:
        fresh = False
    if fresh:
        return destination
    # A previous conversion can leave a locked temporary file (for example
    # after a removable SD reader briefly disconnects).  A process-specific
    # name lets the next export proceed without touching that stale file.
    temporary = destination.with_name(f"{destination.stem}.{os.getpid()}.tmp.mp3")
    try:
        subprocess.run([
            str(ffmpeg), "-hide_banner", "-loglevel", "error", "-nostdin", "-y",
            "-i", str(source), "-map", "0:a:0", "-vn", "-sn", "-dn",
            "-c:a", "libmp3lame", "-b:a", bitrate, "-ar", "44100", "-ac", "2",
            str(temporary),
        ], check=True)
        temporary.replace(destination)
        return destination
    except (OSError, subprocess.CalledProcessError) as error:
        try:
            temporary.unlink(missing_ok=True)
        except OSError:
            pass
        raise RuntimeError(f"AAC performance conversion failed: {source.name}: {error}") from error


def mp3_gapless_delay(data: bytes, frame_start: int, version: int, channels: int, protection: int) -> int:
    """Read the LAME/Xing encoder delay in decoded PCM samples, if present."""
    side_info = (32 if channels > 1 else 17) if version == 3 else (17 if channels > 1 else 9)
    xing = frame_start + 4 + (0 if protection else 2) + side_info
    if data[xing:xing + 4] not in {b"Xing", b"Info"}:
        return -1
    flags = int.from_bytes(data[xing + 4:xing + 8], "big")
    cursor = xing + 8
    if flags & 0x1: cursor += 4
    if flags & 0x2: cursor += 4
    if flags & 0x4: cursor += 100
    if flags & 0x8: cursor += 4
    if data[cursor:cursor + 4] != b"LAME" or cursor + 24 > len(data):
        return -1
    delay = data[cursor + 21:cursor + 24]
    return (delay[0] << 4) | (delay[1] >> 4)


def m4a_gapless_delay(path: Path) -> int:
    """Read Apple/FAAC iTunSMPB priming samples when an AAC file provides it."""
    try:
        data = path.read_bytes()
    except OSError:
        return 0
    marker = data.find(b"iTunSMPB")
    if marker < 0:
        return 0
    match = re.search(rb"([0-9A-Fa-f]{8})\s+([0-9A-Fa-f]{8})\s+([0-9A-Fa-f]{8})", data[marker:marker + 512])
    return int(match.group(2), 16) if match else 0


def mp3_seek_index(path: Path) -> tuple[list[int], list[int], int, int]:
    """Create a fixed, sample-accurate MP3 seek map for the 3DS.

    Each entry starts sixteen whole frames before its target and stores the exact
    number of source samples to discard. This is not a runtime search: a cue
    always decodes the same small preroll and lands at the same sample.
    """
    if path.suffix.lower() != ".mp3":
        return [0] * MP3_SEEK_POINTS, [0] * MP3_SEEK_POINTS, 0, 44100
    try:
        data = path.read_bytes()
    except OSError:
        return [0] * MP3_SEEK_POINTS, [0] * MP3_SEEK_POINTS, 0, 44100
    frames: list[tuple[int, int, int, float]] = []
    gapless_delay = 0
    elapsed_ms, position = 0.0, 0
    bitrate_mpeg1_l3 = (0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320)
    bitrate_mpeg2_l3 = (0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160)
    rates = {3: (44100, 48000, 32000), 2: (22050, 24000, 16000), 0: (11025, 12000, 8000)}

    def header_at(offset: int) -> tuple[int, int, int, int, int, int] | None:
        """Return version/rate/channels/protection/size/samples for a real L3 frame.

        ID3 artwork and arbitrary tag bytes can contain an MP3-looking four
        byte sequence.  Treating one as the first frame poisoned the whole
        3DS seek map (notably with a fake 8 kHz rate in qualia).
        """
        if offset + 4 > len(data):
            return None
        header = int.from_bytes(data[offset:offset + 4], "big")
        version, layer, bitrate_index = (header >> 19) & 3, (header >> 17) & 3, (header >> 12) & 15
        rate_index, padding = (header >> 10) & 3, (header >> 9) & 1
        if (header >> 21) != 0x7FF or version == 1 or layer != 1 or bitrate_index in (0, 15) or rate_index == 3:
            return None
        rate = rates[version][rate_index]
        bitrate = (bitrate_mpeg1_l3 if version == 3 else bitrate_mpeg2_l3)[bitrate_index] * 1000
        frame_size = ((144 if version == 3 else 72) * bitrate // rate) + padding
        if frame_size <= 4 or offset + frame_size > len(data):
            return None
        channels = 1 if ((header >> 6) & 3) == 3 else 2
        protection = (header >> 16) & 1
        return version, rate, channels, protection, frame_size, 1152 if version == 3 else 576

    while position + 4 <= len(data):
        parsed = header_at(position)
        if parsed is None:
            position += 1
            continue
        version, rate, channels, protection, frame_size, samples = parsed
        following = header_at(position + frame_size)
        # The first accepted frame must be followed by a matching real frame.
        # Once locked, reject an accidental resync at a different rate.
        if following is None or following[0] != version or following[1] != rate:
            position += 1
            continue
        if not frames:
            encoded_delay = mp3_gapless_delay(data, position, version, channels, protection)
            # minimp3 emits decoded PCM without a fixed extra presentation
            # delay.  A missing LAME/Xing value is *not* evidence of a 576
            # sample encoder delay; assuming one made low-rate files such as
            # Ultrarhythm start more than 100 ms ahead of the rekordbox grid.
            # Apply only a delay explicitly stored in the stream metadata.
            gapless_delay = max(0, encoded_delay)
        frames.append((position, samples, rate, elapsed_ms))
        elapsed_ms += samples * 1000.0 / rate
        position += frame_size
    if not frames:
        return [0] * MP3_SEEK_POINTS, [0] * MP3_SEEK_POINTS, 0, 44100
    starts = [frame[3] for frame in frames]
    offsets, skips = [], []
    for point in range(MP3_SEEK_POINTS):
        target_ms = point * MP3_SEEK_STEP_MS
        target_frame = max(0, min(len(frames) - 1, bisect.bisect_right(starts, target_ms) - 1))
        # A fixed sixteen-frame preroll provides enough Layer III bit-reservoir
        # history while remaining a bounded, deterministic decode on the 3DS.
        preroll_frame = max(0, target_frame - 16)
        offset, _, rate, target_start_ms = frames[target_frame]
        discard = sum(frame[1] for frame in frames[preroll_frame:target_frame])
        discard += int(round(max(0.0, target_ms - target_start_ms) * rate / 1000.0))
        offsets.append(frames[preroll_frame][0])
        skips.append(min(65535, discard))
    return offsets, skips, gapless_delay, frames[0][2]


def lookup(database: PDBDatabase, method: str, ident: int, field: str, fallback: str = "") -> str:
    try:
        return str(getattr(getattr(database, method)(ident), field))
    except (KeyError, AttributeError):
        return fallback


def colour(red: int, green: int, blue: int) -> int:
    """citro2d's packed colour order is RRGGBBAA in little-endian words."""
    return 0xFF000000 | (red & 0xFF) | ((green & 0xFF) << 8) | ((blue & 0xFF) << 16)


def waveform_rate_from_ext(path: Path) -> int:
    """Return the waveform clock declared by this device-library EXT file.

    PWV3 stores it in u2 as a 16.16-style high word (0x00960000 means
    150 Hz).  The beat grid is already expressed in milliseconds from this
    same analysis origin, so deriving duration from the tag removes a global
    hard-coded timebase from the player cache.
    """
    try:
        with path.open("rb") as stream:
            parsed = AnlzFile.parse_stream(stream)
        tag = next((item for item in parsed.tags if item.type == "PWV3"), None)
        rate = int(tag.content.u2) >> 16 if tag is not None else 0
        return rate if 1 <= rate <= 1000 else REKORDBOX_BIG_WAVE_HZ
    except (OSError, ValueError, KeyError, AttributeError):
        return REKORDBOX_BIG_WAVE_HZ


MEMORY_COLOURS = {
    "pink": colour(255, 105, 180), "red": colour(244, 67, 54),
    "orange": colour(255, 145, 0), "yellow": colour(255, 235, 59),
    "green": colour(0, 210, 110), "aqua": colour(0, 210, 210),
    "blue": colour(55, 110, 255), "purple": colour(170, 90, 255),
}


def memory_colour(database: PDBDatabase, colour_id: int) -> int:
    try:
        return MEMORY_COLOURS.get(str(database.get_color(colour_id).name).casefold(), colour(255, 177, 67))
    except (KeyError, AttributeError):
        return colour(255, 177, 67)


def extended_cues(analysis_path: Path, database: PDBDatabase) -> tuple[list[int], list[int], list[int], list[int]]:
    """Read PCO2/PCP2 directly: it is the only exported cue structure with RGB hot-cue colour."""
    try:
        data = analysis_path.with_suffix(".EXT").read_bytes()
    except OSError:
        return [], [], [], []
    entries: list[tuple[int, int, int, int]] = []
    offset = 0x1C
    while offset + 12 <= len(data):
        tag_size = int.from_bytes(data[offset + 8:offset + 12], "big")
        if tag_size < 12 or offset + tag_size > len(data):
            break
        if data[offset:offset + 4] == b"PCO2" and offset + 20 <= len(data):
            count = int.from_bytes(data[offset + 16:offset + 18], "big")
            cursor = offset + 20
            for _ in range(count):
                if cursor + 44 > offset + tag_size or data[cursor:cursor + 4] != b"PCP2":
                    break
                entry_size = int.from_bytes(data[cursor + 8:cursor + 12], "big")
                if entry_size < 44 or cursor + entry_size > offset + tag_size:
                    break
                hotcue = int.from_bytes(data[cursor + 12:cursor + 16], "big")
                cue_type = int.from_bytes(data[cursor + 16:cursor + 20], "big") >> 24
                time = int.from_bytes(data[cursor + 20:cursor + 24], "big")
                time_end = int.from_bytes(data[cursor + 24:cursor + 28], "big")
                colour_id = data[cursor + 28]
                comment_size = int.from_bytes(data[cursor + 40:cursor + 44], "big")
                colour_offset = cursor + 44 + comment_size
                if hotcue and colour_offset + 4 <= cursor + entry_size:
                    cue_colour = colour(data[colour_offset + 1], data[colour_offset + 2], data[colour_offset + 3])
                else:
                    cue_colour = memory_colour(database, colour_id)
                # PCP2's high byte is 2 for a Loop. A loop Hot Cue is yellow
                # on the player irrespective of its RGB pad colour.
                loop_end = time_end if hotcue and cue_type == 2 and time_end > time else 0xFFFFFFFF
                if loop_end != 0xFFFFFFFF:
                    cue_colour = colour(255, 177, 67)
                entries.append((time, hotcue, cue_colour, loop_end))
                cursor += entry_size
        offset += tag_size
    entries.sort(key=lambda entry: entry[0])
    return ([entry[0] for entry in entries[:16]], [entry[1] for entry in entries[:16]],
            [entry[2] for entry in entries[:16]], [entry[3] for entry in entries[:16]])


def analysis_data(analysis_path: Path, database: PDBDatabase) -> tuple[bytes, list[int], list[int], int, list[int], list[int], list[int], list[int], list[int], list[int]]:
    try:
        analysis = UsbAnlzDatabase()
        analysis.load_dat_file(str(analysis_path))
        points = bytes(int(point) & 0xFF for point in analysis.get_preview_waveform())
        beatgrid = list(analysis.get_beatgrid())[:MAX_BEAT_GRID]
        beats = [int(point.time) for point in beatgrid]
        beat_numbers = [int(point.beat) for point in beatgrid]
        cues = list(analysis.get_cue_points())[:16]
        times = [int(point.time) for point in cues]
        numbers = [int(point.hotcue_number) for point in cues]
        # Full EXT/PCO2 records below are authoritative for loops. DAT-only
        # exports retain ordinary cue positions but carry no dependable colour
        # or loop metadata in this compact cache.
        loop_ends = [0xFFFFFFFF] * len(cues)
        # Device-library cue colours override this value when present.  A cue
        # without an explicit device colour uses the player default: green.
        cue_colours = [colour(45, 218, 105) if number else colour(255, 177, 67) for number in numbers]
        if len(points) == 400:
            ext_path = analysis_path.with_suffix(".EXT")
            waveform_rate = waveform_rate_from_ext(ext_path)
            analysis.load_ext_file(str(ext_path))
            try:
                full_color = [int(point) & 0xFFFF for point in analysis.get_color_waveform()]
            except KeyError:
                # Older device exports have PWV3 but no colour PWV5. Keep their
                # detailed 150-columns-per-second shape rather than falling back
                # to the coarse 400-column overview.
                full_color = [((int(point) & 0x1F) << 2) | (7 << 7)
                              for point in analysis.get_waveform()]
            colors = [full_color[index * len(full_color) // 400] for index in range(400)]
            # Aggregate every source bucket at the runtime pixel density.
            # Taking the strongest analysed column avoids the moving aliasing
            # caused by selecting a different first column at each position.
            # The 3DS default view is 320 pixels across 8 seconds: 40 columns
            # per second. One cache point per column-time prevents 1/2 sample
            # alternation (the source of the local waveform shimmer).
            runtime_count = min(RUNTIME_WAVE_POINTS,
                                max(1, (len(full_color) * RUNTIME_WAVE_HZ + REKORDBOX_BIG_WAVE_HZ - 1) // REKORDBOX_BIG_WAVE_HZ))
            runtime = []
            for index in range(runtime_count):
                begin = index * len(full_color) // runtime_count
                end = max(begin + 1, (index + 1) * len(full_color) // runtime_count)
                runtime.append(max(full_color[begin:end], key=lambda point: (point >> 2) & 0x1F))
            runtime.extend([0] * (RUNTIME_WAVE_POINTS - runtime_count))
            waveform_duration_ms = round(len(full_color) * 1000 / waveform_rate)
            extended_times, extended_numbers, extended_colours, extended_loop_ends = extended_cues(analysis_path, database)
            if extended_times:
                times, numbers, cue_colours, loop_ends = extended_times, extended_numbers, extended_colours, extended_loop_ends
            return points, colors, runtime, waveform_duration_ms, beats, beat_numbers, times, numbers, cue_colours, loop_ends
    except (OSError, KeyError, ValueError):
        pass
    return b"\0" * 400, [0] * 400, [0] * RUNTIME_WAVE_POINTS, 0, [], [], [], [], [], []


def ascii_path_key(path: str) -> str:
    return "".join(char.casefold() for char in path if char.isascii() and char.isalnum())


def write_artwork(source: Path, destination: Path) -> bool:
    try:
        with Image.open(source) as image:
            image = image.convert("RGB").resize((128, 128), Image.Resampling.LANCZOS)
            output = bytearray()
            for red, green, blue in image.getdata():
                rgb565 = ((red >> 3) << 11) | ((green >> 2) << 5) | (blue >> 3)
                output.extend(struct.pack("<H", rgb565))
        destination.write_bytes(output)
        return True
    except (OSError, ValueError):
        return False


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("sd_root", type=Path)
    parser.add_argument("--transcode-aac", action="store_true",
                        help="create 320 kbps MP3 performance copies for every M4A/AAC track")
    parser.add_argument("--ffmpeg", type=Path,
                        help="path to ffmpeg.exe (defaults to ffmpeg on PATH)")
    parser.add_argument("--mp3-bitrate", default="320k", choices=("192k", "256k", "320k"),
                        help="CBR bitrate for --transcode-aac performance copies (default: 320k)")
    parser.add_argument("--skip-artwork", action="store_true",
                        help="reuse existing RGB565 covers; do not decode source artwork")
    arguments = parser.parse_args()
    root = arguments.sd_root.resolve()
    database_path = root / "PIONEER" / "rekordbox" / "export.pdb"
    output_root = root / "3ds" / "3ds_one_deck" / "cache"
    art_root = output_root / "art"
    art_root.mkdir(parents=True, exist_ok=True)
    ffmpeg: Path | None = arguments.ffmpeg
    if arguments.transcode_aac and ffmpeg is None:
        found = shutil.which("ffmpeg")
        ffmpeg = Path(found) if found else None
    if arguments.transcode_aac and (ffmpeg is None or not ffmpeg.is_file()):
        parser.error("--transcode-aac requires ffmpeg.exe; pass --ffmpeg C:\\path\\to\\ffmpeg.exe")

    database = PDBDatabase()
    database.load_file(str(database_path))
    rows: list[bytes] = []
    font_strings: list[str] = []
    known_paths: set[str] = set()
    audio_by_ascii_key: dict[str, list[Path]] = {}
    for audio_path in (root / "Contents").rglob("*"):
        if audio_path.is_file() and audio_path.suffix.lower() in {".mp3", ".m4a", ".aac", ".mp4"}:
            relative = audio_path.relative_to(root).as_posix()
            audio_by_ascii_key.setdefault(ascii_path_key(relative), []).append(audio_path)
    for track in database["tracks"]:
        relative_path = str(track.path).lstrip("/")
        audio_path = root / relative_path
        if not audio_path.is_file():
            matches = audio_by_ascii_key.get(ascii_path_key(relative_path), [])
            if len(matches) == 1:
                audio_path = matches[0]
                relative_path = audio_path.relative_to(root).as_posix()
            elif not matches:
                source_key = ascii_path_key(relative_path)
                scored = sorted([
                    (difflib.SequenceMatcher(None, source_key, key).ratio(), paths)
                    for key, paths in audio_by_ascii_key.items()
                ], key=lambda item: item[0])
                if scored and scored[-1][0] >= 0.45 and len(scored[-1][1]) == 1:
                    audio_path = scored[-1][1][0]
                    relative_path = audio_path.relative_to(root).as_posix()
        if not audio_path.is_file():
            continue
        # The 3DS player only decodes MP3/AAC.  Device backups can also retain
        # rekordbox sample WAV entries, which must not consume the 128-track
        # player cache budget.
        if audio_path.suffix.lower() not in {".mp3", ".m4a", ".aac", ".mp4"}:
            continue
        known_paths.add(relative_path.replace("\\", "/").casefold())
        composer = lookup(database, "get_artist", track.composer_index, "name")
        artist = lookup(database, "get_artist", track.artist_id, "name")
        key = lookup(database, "get_key", track.key_id, "name")
        analysis_path = root / str(track.analyze_path).lstrip("/")
        waveform, colors, runtime_waveform, waveform_duration_ms, beats, beat_numbers, cue_times, cue_numbers, cue_colours, cue_loop_ends = analysis_data(analysis_path, database)
        # Keep the exact timebase of the exported high-resolution waveform.
        # `track.duration` in the PDB is whole seconds; replacing this value
        # with it made the waveform scale very slightly differently from the
        # ANLZ beat-grid, which becomes visible as a steadily growing drift.
        artwork_path = ""
        try:
            artwork = database.get_artwork(track.artwork_id)
            source_art = root / str(artwork.path).lstrip("/")
            cache_art = art_root / f"{track.id}.rgb565"
            if arguments.skip_artwork and cache_art.is_file() and cache_art.stat().st_size == 128 * 128 * 2:
                artwork_path = f"cache/art/{track.id}.rgb565"
            elif write_artwork(source_art, cache_art):
                artwork_path = f"cache/art/{track.id}.rgb565"
        except KeyError:
            pass
        # PDB metadata can contain mojibake for a subset of older exports;
        # the actual exported filename is Unicode and remains reliable.
        title = audio_path.stem if looks_mojibake(track.title) else str(track.title)
        creator = str(composer or artist)
        if looks_mojibake(creator):
            creator = audio_path.parent.name
        display_artist = "" if looks_mojibake(artist) else str(artist)
        font_strings.extend((title, creator, display_artist))
        player_audio = audio_path
        if arguments.transcode_aac and audio_path.suffix.lower() in {".m4a", ".aac", ".mp4"}:
            player_audio = transcode_aac_for_player(audio_path, output_root, str(track.id), ffmpeg,
                                                     arguments.mp3_bitrate)
        seek_offsets, seek_skips, audio_delay_samples, mp3_sample_rate = mp3_seek_index(player_audio)
        if player_audio.suffix.lower() == ".m4a":
            audio_delay_samples = m4a_gapless_delay(player_audio)
            mp3_sample_rate = 0
        rows.append(RECORD.pack(
            fixed(player_audio_path(root, output_root, str(track.id), player_audio), 512),
            fixed(title, 160),
            fixed(creator, 160),
            fixed(display_artist, 120),
            fixed(key, 16),
            fixed(artwork_path, 64),
            int(track.bpm_100), int(track.duration), waveform_duration_ms, audio_delay_samples, int(track.track_number), mp3_sample_rate, waveform, *colors, *runtime_waveform,
            *(beats + [0xFFFFFFFF] * (MAX_BEAT_GRID - len(beats))),
            *(beat_numbers + [0] * (MAX_BEAT_GRID - len(beat_numbers))),
            *(cue_times + [0xFFFFFFFF] * (16 - len(cue_times))),
            *(cue_numbers + [0] * (16 - len(cue_numbers))),
            *(cue_colours + [0] * (16 - len(cue_colours))),
            *([next((end for number, end in zip(cue_numbers, cue_loop_ends) if number == slot),
                     0xFFFFFFFF) for slot in range(1, 9)]),
            *seek_offsets, *seek_skips,
        ))
        if len(rows) >= MAX_TRACKS:
            break

    # Some older PDB exports encode a handful of non-ASCII paths incorrectly.
    # Keep those tracks browseable and playable rather than silently dropping them.
    for audio_path in sorted((root / "Contents").rglob("*")):
        if len(rows) >= MAX_TRACKS:
            break
        if not audio_path.is_file() or audio_path.suffix.lower() not in {".mp3", ".m4a", ".aac", ".mp4"}:
            continue
        relative_path = audio_path.relative_to(root).as_posix()
        if relative_path.casefold() in known_paths:
            continue
        artist = audio_path.parent.parent.name if audio_path.parent.parent != root else ""
        font_strings.extend((audio_path.stem, artist))
        fallback_id = f"fallback_{len(rows):02d}"
        player_audio = audio_path
        if arguments.transcode_aac and audio_path.suffix.lower() in {".m4a", ".aac", ".mp4"}:
            player_audio = transcode_aac_for_player(audio_path, output_root, fallback_id, ffmpeg,
                                                     arguments.mp3_bitrate)
        rows.append(RECORD.pack(
            fixed(player_audio_path(root, output_root, fallback_id, player_audio), 512), fixed(audio_path.stem, 160), fixed(artist, 160),
            fixed(artist, 120), fixed("", 16), fixed("", 64), 0, 0, 0, 0, 0, 0, b"\0" * 400, *([0] * 400),
            *([0] * RUNTIME_WAVE_POINTS), *([0xFFFFFFFF] * MAX_BEAT_GRID), *([0] * MAX_BEAT_GRID),
            *([0xFFFFFFFF] * 16), *([0] * 16), *([0] * 16),
            *([0xFFFFFFFF] * 8),
            *([0] * MP3_SEEK_POINTS), *([0] * MP3_SEEK_POINTS),
        ))

    with (output_root / "library.rbd").open("wb") as stream:
        stream.write(MAGIC)
        stream.write(struct.pack("<I", len(rows)))
        for row in rows:
            stream.write(row)
    build_japanese_font(font_strings, output_root)
    print(f"Wrote {len(rows)} tracks to {output_root / 'library.rbd'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
