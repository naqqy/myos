#!/usr/bin/env python3
import struct, subprocess, sys

MAGIC = 0x5851414E  # NAQX
VERSION = 1

def run(cmd):
    return subprocess.check_output(cmd, text=True)

elf = sys.argv[1]
out = sys.argv[2]

# entry point
hdr = run(["readelf", "-h", elf])
entry = int([l for l in hdr.splitlines() if "Entry point" in l][0].split()[-1], 16)

# raw image
subprocess.check_call(["objcopy", "-O", "binary", elf, "tmp.bin"])
image = open("tmp.bin","rb").read()

# relocations (only R_386_32)
relocs = []
rel = run(["readelf","-r",elf])

for line in rel.splitlines():
    if "R_386_32" in line:
        offset = int(line.split()[0],16)
        relocs.append(offset)

header = struct.pack("<6I",
    MAGIC,
    VERSION,
    len(image),
    entry,
    len(image),   # mem size (same for now)
    len(relocs)
)

with open(out,"wb") as f:
    f.write(header)
    f.write(image)
    for r in relocs:
        f.write(struct.pack("<I",r))