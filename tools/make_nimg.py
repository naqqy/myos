#!/usr/bin/env python3
import os
import struct
import sys

try:
    from PIL import Image
except ImportError:
    print("Error: Pillow is required.", file=sys.stderr)
    print("Install it with: python3 -m pip install pillow", file=sys.stderr)
    sys.exit(1)


NIMG_MAGIC = 0x474D494E  # bytes: N I M G
NIMG_FORMAT_XRGB8888 = 32


def main() -> int:
    if len(sys.argv) != 5:
        print("Usage: make_nimg.py <input.png> <output.ni> <width> <height>", file=sys.stderr)
        return 1

    input_path = sys.argv[1]
    output_path = sys.argv[2]
    width = int(sys.argv[3])
    height = int(sys.argv[4])

    img = Image.open(input_path).convert("RGBA")
    img = img.resize((width, height), Image.LANCZOS)

    # Composite onto black so transparency is safe.
    bg = Image.new("RGBA", (width, height), (0, 0, 0, 255))
    bg.alpha_composite(img)
    img = bg.convert("RGB")

    os.makedirs(os.path.dirname(output_path), exist_ok=True)

    with open(output_path, "wb") as f:
        f.write(struct.pack("<IIII", NIMG_MAGIC, width, height, NIMG_FORMAT_XRGB8888))

        for r, g, b in img.getdata():
            color = (r << 16) | (g << 8) | b
            f.write(struct.pack("<I", color))

    print(f"Wrote {output_path}: {width}x{height}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())