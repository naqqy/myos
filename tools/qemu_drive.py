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
  mouse:X,Y     move the pointer to screen position X,Y (only valid after the
                desktop has started; it places the pointer at the centre)
  click         press and release the left button
  dclick        double-click the left button
  down / up     press / release the left button (for dragging)

example (boot, pass the loader prompt, open the GUI, start an app):
  python3 tools/qemu_drive.py . /tmp/shots \\
    'wait:12;key:ret;wait:6;type:gui;key:ret;wait:2;type:spawn cube;key:ret;wait:4;shot:gui'

Set QEMU_RAM (default 1024M) to test other memory sizes.

Pointer input: by default QEMU offers the guest an absolute pointer (vmmouse),
which is what the normal QEMU window uses, and the mouse steps send absolute
positions. Set QEMU_MOUSE=rel to disable it (-machine vmport=off) and send
relative PS/2 motion instead, like a physical mouse on real hardware.
"""
import json, os, shutil, socket, subprocess, sys, time
from PIL import Image

proj, out, script = sys.argv[1], sys.argv[2], sys.argv[3]
ram = os.environ.get("QEMU_RAM", "1024M")
os.makedirs(out, exist_ok=True)
work = "/tmp/naq_qemu"
os.makedirs(work, exist_ok=True)
shutil.copy("/usr/share/OVMF/OVMF_VARS_4M.fd", work + "/vars.fd")
shutil.copy(proj + "/build/uefi.img", work + "/uefi.img")

PORT = 45454
QMP_PORT = 45455
RELATIVE_MOUSE = os.environ.get("QEMU_MOUSE", "abs") == "rel"
SCREEN_W = int(os.environ.get("QEMU_SCREEN_W", "1280"))
SCREEN_H = int(os.environ.get("QEMU_SCREEN_H", "800"))

cmd = [
    "qemu-system-x86_64",
    "-machine", "pc,vmport=off" if RELATIVE_MOUSE else "pc",
    "-m", ram, "-cpu", "qemu64",
    "-drive", "if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd",
    "-drive", "if=pflash,format=raw,file=" + work + "/vars.fd",
    "-drive", "if=ide,format=raw,file=" + work + "/uefi.img",
    "-vga", "qxl", "-global", "VGA.vgamem_mb=64",
    "-display", "none", "-serial", "none", "-net", "none",
    "-no-reboot", "-d", "cpu_reset,guest_errors", "-D", work + "/qemu.log",
    "-monitor", "tcp:127.0.0.1:%d,server,nowait" % PORT,
    "-qmp", "tcp:127.0.0.1:%d,server,nowait" % QMP_PORT,
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


def send(line):
    """Send a monitor command without waiting for its output (fast path for
    keys and mouse input, where timing matters)."""
    s.sendall((line + chr(10)).encode())
    time.sleep(0.03)


KEYS = {" ": "spc", "/": "slash", ".": "dot", "-": "minus", "_": "shift-minus",
        "\n": "ret"}


def send_char(c):
    if c in KEYS:
        k = KEYS[c]
    elif c.isupper():
        k = "shift-" + c.lower()
    else:
        k = c
    send("sendkey " + k)


qmp_sock = None


def qmp(command, arguments=None):
    """Run one QMP command (used for absolute pointer input, which the
    human monitor cannot send)."""
    global qmp_sock
    if qmp_sock is None:
        qmp_sock = socket.create_connection(("127.0.0.1", QMP_PORT))
        qmp_sock.settimeout(1.0)
        qmp_sock.recv(65536)  # greeting
        qmp_sock.sendall(b'{"execute": "qmp_capabilities"}' + bytes([10]))
        qmp_sock.recv(65536)
    msg = {"execute": command}
    if arguments is not None:
        msg["arguments"] = arguments
    qmp_sock.sendall(json.dumps(msg).encode() + bytes([10]))
    time.sleep(0.03)
    try:
        qmp_sock.recv(65536)
    except socket.timeout:
        pass


# The desktop starts the pointer in the middle of the screen.
mouse_x = SCREEN_W // 2
mouse_y = SCREEN_H // 2


def pointer_move(tx, ty):
    """Move the pointer to a screen position in small steps, so the guest
    sees motion the way it would from a hand-moved mouse."""
    global mouse_x, mouse_y
    while (mouse_x, mouse_y) != (tx, ty):
        dx = max(-40, min(40, tx - mouse_x))
        dy = max(-40, min(40, ty - mouse_y))
        mouse_x += dx
        mouse_y += dy
        if RELATIVE_MOUSE:
            send("mouse_move %d %d" % (dx, dy))
        else:
            qmp("input-send-event", {"events": [
                {"type": "abs", "data": {
                    "axis": "x", "value": mouse_x * 0x7FFF // (SCREEN_W - 1)}},
                {"type": "abs", "data": {
                    "axis": "y", "value": mouse_y * 0x7FFF // (SCREEN_H - 1)}},
            ]})


def pointer_button(down):
    if RELATIVE_MOUSE:
        send("mouse_button %d" % (1 if down else 0))
    else:
        qmp("input-send-event", {"events": [
            {"type": "btn", "data": {"down": bool(down), "button": "left"}},
        ]})

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
            send("sendkey " + arg)
        elif op == "hmp":
            print(hmp(arg))
        elif op == "mouse":
            tx, ty = (int(v) for v in arg.split(","))
            pointer_move(tx, ty)
        elif op == "down":
            pointer_button(True)
        elif op == "up":
            pointer_button(False)
        elif op == "click":
            pointer_button(True)
            time.sleep(0.06)
            pointer_button(False)
        elif op == "dclick":
            for _ in range(2):
                pointer_button(True)
                time.sleep(0.04)
                pointer_button(False)
                time.sleep(0.04)
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
