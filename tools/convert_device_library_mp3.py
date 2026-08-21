"""Make a 3DS-optimised MP3 performance library from a rekordbox SD export.

The original Device Library is never modified.  AAC/M4A/MP4 files are converted
to deterministic 44.1 kHz stereo CBR MP3 files under the CDJ-3DS cache, then a
fresh ``library.rbd`` is generated which points the player at those copies while
retaining rekordbox metadata, artwork, beat grids and cue positions.
"""
from __future__ import annotations

import argparse
from pathlib import Path
import shutil
import subprocess
import sys


def find_ffmpeg(explicit: Path | None) -> Path:
    if explicit is not None:
        if explicit.is_file():
            return explicit
        raise FileNotFoundError(f"ffmpeg.exe was not found: {explicit}")
    found = shutil.which("ffmpeg")
    if found:
        return Path(found)
    raise FileNotFoundError(
        "ffmpeg.exe was not found. Install FFmpeg or pass --ffmpeg C:\\path\\to\\ffmpeg.exe")


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Convert rekordbox AAC/M4A files to CDJ-3DS MP3 performance copies")
    parser.add_argument("sd_root", type=Path,
                        help="root of the rekordbox Device Library SD card, e.g. F:\\")
    parser.add_argument("--ffmpeg", type=Path, help="path to ffmpeg.exe")
    parser.add_argument("--bitrate", default="320k", choices=("192k", "256k", "320k"),
                        help="MP3 CBR bitrate (default: 320k)")
    parser.add_argument("--skip-artwork", action="store_true",
                        help="reuse existing CDJ-3DS RGB565 artwork cache")
    args = parser.parse_args()

    root = args.sd_root.resolve()
    if not (root / "PIONEER" / "rekordbox" / "export.pdb").is_file():
        parser.error(f"rekordbox export.pdb was not found under: {root}")
    if not (root / "Contents").is_dir():
        parser.error(f"Contents directory was not found under: {root}")

    try:
        ffmpeg = find_ffmpeg(args.ffmpeg)
    except FileNotFoundError as error:
        parser.error(str(error))

    cache_builder = Path(__file__).with_name("build_rekordbox_cache.py")
    command = [
        sys.executable, str(cache_builder), str(root),
        "--transcode-aac", "--ffmpeg", str(ffmpeg), "--mp3-bitrate", args.bitrate,
    ]
    if args.skip_artwork:
        command.append("--skip-artwork")
    try:
        subprocess.run(command, check=True)
    except subprocess.CalledProcessError as error:
        return error.returncode or 1

    performance_root = root / "3ds" / "3ds_one_deck" / "cache" / "audio" / "performance"
    converted = len(list(performance_root.glob("*.mp3"))) if performance_root.is_dir() else 0
    print(f"Ready: {converted} MP3 performance copies in {performance_root}")
    print("CDJ-3DS will use these MP3 files; PIONEER and original audio files are unchanged.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
