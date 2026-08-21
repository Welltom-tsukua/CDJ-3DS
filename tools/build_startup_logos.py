#!/usr/bin/env python3
"""Build clean RGB565 boot logos for the 3DS's black startup screens.

The supplied Pioneer image has an alpha matte, while the public rekordbox
image is white-on-black.  Normalising both here avoids JPEG near-black halos
that become visible on the 3DS LCD when the image is drawn over #000000.
"""

from pathlib import Path
import struct
from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
ASSETS = ROOT / "assets"
ROMFS = ROOT / "romfs"
CANVAS = (256, 128)
PIONEER_RED = (218, 10, 55)


def crop_alpha(image: Image.Image) -> Image.Image:
    image = image.convert("RGBA")
    alpha = image.getchannel("A")
    box = alpha.getbbox()
    return image.crop(box) if box else image


def pioneer() -> Image.Image:
    source = crop_alpha(Image.open(ASSETS / "pioneer_dj_logo.png"))
    # Preserve the supplied antialiasing, but make the visible mark the
    # official red rather than retaining a dark JPEG/preview matte.
    alpha = source.getchannel("A")
    mark = Image.new("RGBA", source.size, PIONEER_RED + (0,))
    mark.putalpha(alpha)
    return mark


def rekordbox() -> Image.Image:
    source = Image.open(ASSETS / "rekordbox_logo_source.png").convert("L")
    # Anything close to the source's black field becomes fully transparent;
    # the original bright edge remains as smooth alpha.
    alpha = source.point(lambda value: 0 if value < 24 else value)
    box = alpha.getbbox()
    alpha = alpha.crop(box) if box else alpha
    mark = Image.new("RGBA", alpha.size, (245, 245, 245, 0))
    mark.putalpha(alpha)
    return mark


def fit_on_black(mark: Image.Image) -> Image.Image:
    max_w, max_h = CANVAS[0] - 12, CANVAS[1] - 24
    factor = min(max_w / mark.width, max_h / mark.height)
    size = (max(1, round(mark.width * factor)), max(1, round(mark.height * factor)))
    mark = mark.resize(size, Image.Resampling.LANCZOS)
    canvas = Image.new("RGB", CANVAS, (0, 0, 0))
    canvas.paste(mark, ((CANVAS[0] - mark.width) // 2, (CANVAS[1] - mark.height) // 2), mark)
    return canvas


def rgb565(image: Image.Image, destination: Path) -> None:
    raw = bytearray()
    for red, green, blue in image.convert("RGB").getdata():
        raw.extend(struct.pack("<H", ((red >> 3) << 11) | ((green >> 2) << 5) | (blue >> 3)))
    destination.write_bytes(raw)


def main() -> None:
    ROMFS.mkdir(exist_ok=True)
    rgb565(fit_on_black(pioneer()), ROMFS / "PioneerDJLogo.rgb565")
    rgb565(fit_on_black(rekordbox()), ROMFS / "rekordboxLogo.rgb565")


if __name__ == "__main__":
    main()
