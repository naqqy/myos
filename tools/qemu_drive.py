#!/usr/bin/env python3
"""Boot build/uefi.img headless in QEMU, type shell commands, save screenshots.

Run inside WSL/Linux after ./build.sh uefi (needs qemu-system-x86, ovmf,
python3-pil). The image is copied to /tmp/naq_qemu first, so the build output
is never modified.

usage: qemu_drive.py <project_dir> <out_dir> <script>

script is a semicolon-separated list of steps:
  wait:N        sleep N seconds
  type:text     type text on the emulated keyboard
  key:NAME      press one key (QEMU names: ret, tab, esc, ...);
                "key:ret 40" holds it for 40 ms
  shot:NAME     save a screenshot to <out_dir>/NAME.png
  hmp:COMMAND   run a QEMU monitor command and print its output

example (boot, pass the loader prompt, open the GUI, start an app):
  python3 tools/qemu_drive.py . /tmp/shots \\
    'wait:12;key:ret;wait:6;type:gui;key:ret;wait:2;type:spawn cube;key:ret;wait:4;shot:gui'

Set QEMU_RAM (default 1024M) to test other memory sizes.
"""
import os, shutil, socket, subprocess, sys, time
from PIL import Image

proj, out, script = sys.argv[1], sys.argv[2], sys.argv[3]
ram = os.environ.get("QEMU_RAM", "1024M")
os.makedirs(out, exist_ok=True)
work = "/tmp/naq_qemu"
os.makedirs(work, exist_ok=True)
shutil.copy("/usr/share/OVMF/OVMF_VARS_4M.fd", work + "/vars.fd")
shutil.copy(proj + "/build/uefi.img", work + "/uefi.img")

PORT = 45454
cmd = [
    "qemu-system-x86_64", "-machine", "pc", "-m", ram, "-cpu", "qemu64",
    "-drive", "if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd",
    "-drive", "if=pflash,format=raw,file=" + work + "/vars.fd",
    "-drive", "if=ide,format=raw,file=" + work + "/uefi.img",
    "-vga", "qxl", "-global", "VGA.vgamem_mb=64",
    "-display", "none", "-serial", "none", "-net", "none",
    "-no-reboot", "-d", "cpu_reset,guest_errors", "-D", work + "/qemu.log",
    "-monitor", "tcp:127.0.0.1:%d,server,nowait" % PORT,
]
p = subprocess.Popen(cmd)
time.sleep(1.5)
s = socket.create_connection(("127.0.0.1", PORT))
s.settimeout(0.5)


def hmp(line):
    s.sendall((line + "\n").encode())
    time.sleep(0.08)
    out = b""
    try:
        while True:
            d = s.recv(65536)
            if not d:
                break
            out += d
    except socket.timeout:
        pass
    return out.decode(errors="replace")


KEYS = {" ": "spc", "/": "slash", ".": "dot", "-": "minus", "_": "shift-minus",
        "\n": "ret"}


def send_char(c):
    if c in KEYS:
        k = KEYS[c]
    elif c.isupper():
        k = "shift-" + c.lower()
    else:
        k = c
    hmp("sendkey " + k)


try:
    for step in script.split(";"):
        step = step.strip()
        if not step:
            continue
        op, _, arg = step.partition(":")
        if op == "wait":
            time.sleep(float(arg))
        elif op == "type":
            for c in arg:
                send_char(c)
        elif op == "key":
            hmp("sendkey " + arg)
        elif op == "hmp":
            print(hmp(arg))
        elif op == "shot":
            ppm = work + "/" + arg + ".ppm"
            hmp("screendump " + ppm)
            time.sleep(0.6)
            Image.open(ppm).save(os.path.join(out, arg + ".png"))
            print("shot", arg, Image.open(ppm).size)
        if p.poll() is not None:
            print("QEMU EXITED rc=", p.returncode)
            break
finally:
    try:
        hmp("quit")
    except Exception:
        pass
    time.sleep(0.5)
    if p.poll() is None:
        p.kill()
    log = open(work + "/qemu.log").read()
    print("qemu.log bytes:", len(log), "resets:", log.count("CPU Reset"))
