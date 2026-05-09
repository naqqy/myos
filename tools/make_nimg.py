#!/usr/bin/env python3
import os
import struct
import sys

try:
    from PIL import Image
except ImportError:
    print("Error: Pillow is required.", file=sys.stderr)
    print("Install it with: sudo apt install python3-pil", file=sys.stderr)
    sys.exit(1)


NIMG_MAGIC = 0x474D494E  # "NIMG"
NIMG_FORMAT_XRGB8888 = 32


def main() -> int:
    if len(sys.argv) not in (3, 5):
        print("Usage: make_nimg.py <input.png> <output.ni> [width height]", file=sys.stderr)
        return 1

    input_path = sys.argv[1]
    output_path = sys.argv[2]

    img = Image.open(input_path).convert("RGBA")

    if len(sys.argv) == 5:
        width = int(sys.argv[3])
        height = int(sys.argv[4])
    else:
        width, height = img.size

    if width <= 0 or height <= 0:
        print("Error: width and height must be positive.", file=sys.stderr)
        return 1

    try:
        resample = Image.Resampling.LANCZOS
    except AttributeError:
        resample = Image.LANCZOS

    img = img.resize((width, height), resample)

    bg = Image.new("RGBA", (width, height), (0, 0, 0, 255))
    bg.alpha_composite(img)
    img = bg.convert("RGB")

    output_dir = os.path.dirname(output_path)
    if output_dir:
        os.makedirs(output_dir, exist_ok=True)

    with open(output_path, "wb") as f:
        f.write(struct.pack("<IIII", NIMG_MAGIC, width, height, NIMG_FORMAT_XRGB8888))

        for r, g, b in img.getdata():
            color = (r << 16) | (g << 8) | b
            f.write(struct.pack("<I", color))

    print(f"Wrote {output_path}: {width}x{height}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())