#!/usr/bin/env python3
import os
import struct
import sys

INITRD_MAGIC = 0x4452494E  # "NIRD"
INITRD_VERSION = 1
MAX_PATH = 128

def collect_files(root):
    files = []

    for dirpath, _, filenames in os.walk(root):
        for name in filenames:
            full = os.path.join(dirpath, name)
            rel = os.path.relpath(full, root).replace("\\", "/")
            rel = "/" + rel.lstrip("/")

            if len(rel.encode("utf-8")) >= MAX_PATH:
                raise RuntimeError(f"path too long for initrd: {rel}")

            files.append((rel, full))

    files.sort()
    return files

def main() -> int:
    if len(sys.argv) != 3:
        print("Usage: make_initrd.py <input_dir> <output.naq>", file=sys.stderr)
        return 1

    input_dir = sys.argv[1]
    output_path = sys.argv[2]

    if not os.path.isdir(input_dir):
        print(f"Error: input dir not found: {input_dir}", file=sys.stderr)
        return 1

    files = collect_files(input_dir)

    header_size = 16
    entry_size = MAX_PATH + 8 + 8
    data_offset = header_size + entry_size * len(files)

    entries = []
    data_blobs = []

    current_offset = data_offset

    for rel, full in files:
        with open(full, "rb") as f:
            data = f.read()

        entries.append((rel, current_offset, len(data)))
        data_blobs.append(data)
        current_offset += len(data)

        if current_offset % 16:
            pad = 16 - (current_offset % 16)
            current_offset += pad

    out_dir = os.path.dirname(output_path)
    if out_dir:
        os.makedirs(out_dir, exist_ok=True)

    with open(output_path, "wb") as out:
        out.write(struct.pack("<IIII", INITRD_MAGIC, INITRD_VERSION, len(files), header_size))

        for rel, offset, size in entries:
            path_bytes = rel.encode("utf-8")
            path_field = path_bytes + b"\x00" * (MAX_PATH - len(path_bytes))
            out.write(path_field)
            out.write(struct.pack("<QQ", offset, size))

        for data in data_blobs:
            out.write(data)
            if out.tell() % 16:
                out.write(b"\x00" * (16 - (out.tell() % 16)))

    print(f"Wrote {output_path}: {len(files)} files")
    return 0

if __name__ == "__main__":
    raise SystemExit(main())