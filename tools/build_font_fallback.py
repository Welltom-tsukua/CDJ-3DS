"""Build only the Japanese glyphs currently needed by a 3DS One Deck cache.

This intentionally reads library.rbd directly, so it can repair the font even
when rekordbox/PDB Python dependencies are unavailable on the PC.
"""
from __future__ import annotations

import argparse
import subprocess
from pathlib import Path


HEADER_SIZE = 12
RECORD_SIZE = 137568


def c_string(value: bytes) -> str:
    return value.split(b"\0", 1)[0].decode("utf-8", "replace")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("cache", type=Path)
    parser.add_argument("font", type=Path)
    parser.add_argument("mkbcfnt", type=Path)
    args = parser.parse_args()
    cache = args.cache.resolve()
    output = cache.parent
    raw = cache.read_bytes()
    if raw[:8] != b"RB3D14\0\0":
        raise ValueError("not a 3DS One Deck library cache")
    count = int.from_bytes(raw[8:12], "little")
    glyphs = set(range(0x20, 0x7F))
    glyphs.update(range(0x3000, 0x3040))  # Japanese punctuation, including 【】
    glyphs.update({0x00B1, 0x2190, 0x2192, 0x2212, 0x25B6, 0x25C0})
    for number in range(count):
        start = HEADER_SIZE + number * RECORD_SIZE
        if start + RECORD_SIZE > len(raw):
            break
        # path/title/composer/artist/key are the first five fixed fields.
        fields = ((512, 160), (672, 160), (832, 120), (952, 16))
        for offset, size in fields:
            glyphs.update(ord(char) for char in c_string(raw[start + offset:start + offset + size]))
    glyph_file = output / "NotoSansJP-codepoints.txt"
    glyph_file.write_text("\n".join(f"0x{codepoint:04X}" for codepoint in sorted(glyphs)), encoding="ascii")
    subprocess.run([str(args.mkbcfnt), "-s", "22", "-w", str(glyph_file), "-o",
                    str(output / "NotoSansJP.bcfnt"), str(args.font)], check=True)
    print(f"Wrote {len(glyphs)} glyphs to {output / 'NotoSansJP.bcfnt'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
