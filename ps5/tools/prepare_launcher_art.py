#!/usr/bin/env python3
"""Build neutral launcher artwork and the menu font atlas from local inputs."""
import argparse
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("destination", type=Path)
    parser.add_argument("--background", type=Path)
    args = parser.parse_args()
    root = args.destination
    (root / "sce_sys").mkdir(parents=True, exist_ok=True)

    art = Image.new("RGBA", (640, 480), "#181526")
    draw = ImageDraw.Draw(art)
    draw.rectangle((0, 0, 640, 6), fill="#b6df4d")
    draw.text((24, 72), "The Nightmare Before Christmas", font=ImageFont.load_default(size=25), fill="#fbe5b7")
    draw.text((24, 116), "Oogie's Revenge", font=ImageFont.load_default(size=29), fill="#fbe5b7")
    draw.text((24, 168), "PS5 Port", font=ImageFont.load_default(size=17), fill="#b6df4d")
    if args.background:
        with Image.open(args.background) as custom:
            art = custom.convert("RGBA").resize((640, 480), Image.Resampling.LANCZOS)

    with (root / "setup-menu.bin").open("wb") as out:
        out.write(art.tobytes("raw", "BGRA"))
        font = ImageFont.load_default(size=17)
        for code in range(32, 127):
            glyph = Image.new("L", (15, 27))
            ImageDraw.Draw(glyph).text((0, 2), chr(code), font=font, fill=255)
            out.write(glyph.tobytes())

    icon = Image.new("RGBA", (512, 512), "#181526")
    draw = ImageDraw.Draw(icon)
    draw.rounded_rectangle((45, 45, 466, 466), radius=64, outline="#b6df4d", width=11)
    draw.text((256, 215), "OOGIE", anchor="mm", font=ImageFont.load_default(size=84), fill="#fbe5b7")
    draw.text((256, 311), "PS5 PORT", anchor="mm", font=ImageFont.load_default(size=40), fill="#b6df4d")
    icon.save(root / "sce_sys" / "icon0.png")


if __name__ == "__main__":
    main()
