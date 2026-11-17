#!/usr/bin/env python3
"""Convert a Wavefront OBJ model into the NAQ OS NMODEL format.

usage: make_nmodel.py <input.obj> <output.nm> [--no-fit]

NMODEL v1 (little-endian, every block padded to 4 bytes):

    header, 32 bytes
        u32 magic         "NMDL"
        u32 version       1
        u32 flags         bit 0: has texture coordinates
        u32 vertex_count
        u32 tri_count
        u32 uv_count      0 when flag bit 0 is clear
        u32 reserved[2]
    vertices   vertex_count * { i32 x, y, z }     20.12 fixed point
    uvs        uv_count     * { i32 u, v }        20.12 fixed point, 0..1
    tris       tri_count    * { u16 a, b, c }     vertex indices
    tri_uvs    tri_count    * { u16 a, b, c }     uv indices (flag bit 0)
    tri_colors tri_count    * u32                 0x00RRGGBB

Conventions the converter applies:
  * Polygons are fan-triangulated.
  * OBJ is right-handed (+Z toward the viewer); the renderer is left-handed
    (+Z into the screen), so Z is negated. With that, OBJ's counter-clockwise
    front faces are front faces for r3d back-face culling.
  * V is flipped (OBJ v=0 is the bottom of the image, NIMG row 0 is the top).
  * Unless --no-fit is given the model is centred and scaled so its largest
    half-extent is 1.0, the same size as the built-in cube.
  * Triangle colours come from the material's Kd in the .mtl file when there
    is one, otherwise from a small default palette indexed by material.
"""
import os
import struct
import sys

NMODEL_MAGIC = 0x4C444D4E  # "NMDL"
NMODEL_VERSION = 1
NMODEL_FLAG_UVS = 1

FP_ONE = 1 << 12

MAX_VERTICES = 1024
MAX_UVS = 65535
MAX_TRIS = 65535

DEFAULT_PALETTE = [
    0x0000D8FF, 0x00FFE35A, 0x00FF7A7A, 0x0000FF9A,
    0x00BDA0FF, 0x00B8FF4A, 0x00FFA040, 0x00E0E0E8,
]


def to_fp(value):
    return int(round(value * FP_ONE))


def load_mtl(path):
    colors = {}
    current = None

    try:
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            for line in f:
                parts = line.split()

                if not parts:
                    continue

                if parts[0] == "newmtl" and len(parts) > 1:
                    current = parts[1]
                elif parts[0] == "Kd" and current and len(parts) >= 4:
                    r, g, b = (max(0.0, min(1.0, float(x))) for x in parts[1:4])
                    colors[current] = (
                        (int(r * 255) << 16) | (int(g * 255) << 8) | int(b * 255)
                    )
    except OSError:
        pass

    return colors


def resolve_index(text, count):
    index = int(text)

    if index < 0:
        index = count + index
    else:
        index -= 1

    if index < 0 or index >= count:
        raise ValueError(f"index {text} out of range")

    return index


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    fit = "--no-fit" not in sys.argv

    if len(args) != 2:
        print(__doc__, file=sys.stderr)
        return 1

    src, dst = args

    vertices = []
    uvs = []
    tris = []       # (a, b, c)
    tri_uvs = []    # (a, b, c) or None
    tri_colors = []

    mtl_colors = {}
    materials = []
    current_material = None

    with open(src, "r", encoding="utf-8", errors="replace") as f:
        for line_no, line in enumerate(f, 1):
            parts = line.split()

            if not parts or parts[0].startswith("#"):
                continue

            tag = parts[0]

            try:
                if tag == "v":
                    vertices.append(tuple(float(x) for x in parts[1:4]))
                elif tag == "vt":
                    u = float(parts[1])
                    v = float(parts[2]) if len(parts) > 2 else 0.0
                    uvs.append((u, v))
                elif tag == "mtllib" and len(parts) > 1:
                    mtl_colors.update(
                        load_mtl(os.path.join(os.path.dirname(src), parts[1]))
                    )
                elif tag == "usemtl" and len(parts) > 1:
                    current_material = parts[1]

                    if current_material not in materials:
                        materials.append(current_material)
                elif tag == "f":
                    corners = []

                    for item in parts[1:]:
                        fields = item.split("/")
                        vi = resolve_index(fields[0], len(vertices))
                        ti = None

                        if len(fields) > 1 and fields[1]:
                            ti = resolve_index(fields[1], len(uvs))

                        corners.append((vi, ti))

                    if len(corners) < 3:
                        continue

                    if current_material in mtl_colors:
                        color = mtl_colors[current_material]
                    else:
                        slot = (
                            materials.index(current_material)
                            if current_material in materials
                            else 0
                        )
                        color = DEFAULT_PALETTE[slot % len(DEFAULT_PALETTE)]

                    for i in range(1, len(corners) - 1):
                        fan = (corners[0], corners[i], corners[i + 1])
                        tris.append(tuple(c[0] for c in fan))

                        if all(c[1] is not None for c in fan):
                            tri_uvs.append(tuple(c[1] for c in fan))
                        else:
                            tri_uvs.append(None)

                        tri_colors.append(color)
            except (ValueError, IndexError) as e:
                print(f"{src}:{line_no}: {e}", file=sys.stderr)
                return 1

    if not vertices or not tris:
        print(f"{src}: no geometry found", file=sys.stderr)
        return 1

    if len(vertices) > MAX_VERTICES:
        print(
            f"{src}: {len(vertices)} vertices, renderer limit is {MAX_VERTICES}",
            file=sys.stderr,
        )
        return 1

    if len(tris) > MAX_TRIS or len(uvs) > MAX_UVS:
        print(f"{src}: too many triangles or texture coordinates", file=sys.stderr)
        return 1

    has_uvs = bool(uvs) and all(t is not None for t in tri_uvs)

    if fit:
        mins = [min(v[i] for v in vertices) for i in range(3)]
        maxs = [max(v[i] for v in vertices) for i in range(3)]
        centre = [(mins[i] + maxs[i]) / 2.0 for i in range(3)]
        half = max((maxs[i] - mins[i]) / 2.0 for i in range(3)) or 1.0
        vertices = [
            tuple((v[i] - centre[i]) / half for i in range(3)) for v in vertices
        ]

    out = bytearray()
    out += struct.pack(
        "<8I",
        NMODEL_MAGIC,
        NMODEL_VERSION,
        NMODEL_FLAG_UVS if has_uvs else 0,
        len(vertices),
        len(tris),
        len(uvs) if has_uvs else 0,
        0,
        0,
    )

    for x, y, z in vertices:
        out += struct.pack("<3i", to_fp(x), to_fp(y), to_fp(-z))

    if has_uvs:
        for u, v in uvs:
            out += struct.pack("<2i", to_fp(u), to_fp(1.0 - v))

    def pad4():
        while len(out) % 4:
            out.append(0)

    for a, b, c in tris:
        out += struct.pack("<3H", a, b, c)

    pad4()

    if has_uvs:
        for a, b, c in tri_uvs:
            out += struct.pack("<3H", a, b, c)

        pad4()

    for color in tri_colors:
        out += struct.pack("<I", color & 0x00FFFFFF)

    out_dir = os.path.dirname(dst)

    if out_dir:
        os.makedirs(out_dir, exist_ok=True)

    with open(dst, "wb") as f:
        f.write(out)

    print(
        f"Wrote {dst}: {len(vertices)} vertices, {len(tris)} triangles, "
        f"{'with' if has_uvs else 'no'} UVs, {len(out)} bytes"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
