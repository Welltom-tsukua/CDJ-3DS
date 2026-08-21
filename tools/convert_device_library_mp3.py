"""Create a 3DS playback replacement map for rekordbox AAC/M4A exports.

This is intentionally separate from device-library import.  It never changes
PIONEER/export.pdb or the source AAC files.  The 3DS importer can read the
map and select the generated MP3 only for audio playback, while all metadata,
beat-grid and cue information continues to come from the original export.
"""
from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import sys


MAP_HEADER = "RB3D_AUDIO_MAP\t1\n"


def find_ffmpeg(explicit: Path | None) -> Path:
    if explicit:
        if explicit.is_file():
            return explicit
        raise FileNotFoundError(f"ffmpeg not found: {explicit}")
    found = shutil.which("ffmpeg")
    if found:
        return Path(found)
    local = Path(os.environ.get("LOCALAPPDATA", ""))
    packages = local / "Microsoft" / "WinGet" / "Packages"
    matches = sorted(packages.glob("Gyan.FFmpeg*/*/bin/ffmpeg.exe"))
    if matches:
        return matches[-1]
    raise FileNotFoundError("ffmpeg.exe was not found; install FFmpeg or pass --ffmpeg")


def replacement_name(relative: str) -> str:
    # Hashing preserves stable identity even when exported titles contain
    # Japanese or reserved FAT filename characters.
    return hashlib.sha1(relative.encode("utf-8")).hexdigest()[:20] + ".mp3"


def convert(source: Path, destination: Path, ffmpeg: Path, bitrate: str) -> None:
    try:
        current = destination.is_file() and destination.stat().st_size > 0
        current = current and destination.stat().st_mtime_ns >= source.stat().st_mtime_ns
    except OSError:
        current = False
    if current:
        return
    destination.parent.mkdir(parents=True, exist_ok=True)
    temporary = destination.with_name(f".{destination.stem}.{os.getpid()}.tmp.mp3")
    subprocess.run([
        str(ffmpeg), "-hide_banner", "-loglevel", "error", "-nostdin", "-y",
        "-i", str(source), "-map", "0:a:0", "-vn", "-sn", "-dn",
        "-c:a", "libmp3lame", "-b:a", bitrate, "-ar", "44100", "-ac", "2",
        str(temporary),
    ], check=True)
    temporary.replace(destination)


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Generate MP3 performance replacements without changing rekordbox exports")
    parser.add_argument("sd_root", type=Path, help="root of the rekordbox device-export SD card")
    parser.add_argument("--ffmpeg", type=Path, help="path to ffmpeg.exe")
    parser.add_argument("--bitrate", default="320k", choices=("192k", "256k", "320k"),
                        help="MP3 CBR bitrate (default: 320k)")
    args = parser.parse_args()
    root = args.sd_root.resolve()
    contents = root / "Contents"
    if not contents.is_dir():
        parser.error(f"Contents directory not found: {contents}")
    ffmpeg = find_ffmpeg(args.ffmpeg)
    replacement_root = root / "3ds" / "3ds_one_deck" / "cache" / "performance_audio"
    entries: list[tuple[str, str]] = []
    for source in sorted(contents.rglob("*")):
        if not source.is_file() or source.suffix.lower() not in {".m4a", ".aac", ".mp4"}:
            continue
        original = source.relative_to(root).as_posix()
        replacement = replacement_root / replacement_name(original)
        try:
            convert(source, replacement, ffmpeg, args.bitrate)
        except (OSError, subprocess.CalledProcessError) as error:
            print(f"ERROR: {source.name}: {error}", file=sys.stderr)
            return 1
        entries.append((original, replacement.relative_to(root).as_posix()))
        print(f"MP3 {len(entries):03d}: {source.name}")
    map_path = root / "3ds" / "3ds_one_deck" / "cache" / "performance-audio-map.tsv"
    map_path.parent.mkdir(parents=True, exist_ok=True)
    temporary_map = map_path.with_suffix(".tmp")
    with temporary_map.open("w", encoding="utf-8", newline="\n") as stream:
        stream.write(MAP_HEADER)
        for original, replacement in entries:
            stream.write(f"{original}\t{replacement}\n")
    temporary_map.replace(map_path)
    print(f"Wrote {len(entries)} replacements to {map_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
