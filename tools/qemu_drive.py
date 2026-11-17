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
Set QEMU_ACCEL=kvm to run with hardware virtualization (needs access to
/dev/kvm); the default is software emulation, which is slower but always works.

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
# QEMU_KEEP_DISK=1 reuses the disk from the previous run, to test things the
# guest saved on it (the loader's display choice, for example).
if not (os.environ.get("QEMU_KEEP_DISK") == "1" and os.path.exists(work + "/uefi.img")):
    shutil.copy(proj + "/build/uefi.img", work + "/uefi.img")

PORT = 45454
QMP_PORT = 45455
RELATIVE_MOUSE = os.environ.get("QEMU_MOUSE", "abs") == "rel"
SCREEN_W = int(os.environ.get("QEMU_SCREEN_W", "1280"))
SCREEN_H = int(os.environ.get("QEMU_SCREEN_H", "800"))

cmd = [
    "qemu-system-x86_64",
    "-machine", "pc,vmport=off" if RELATIVE_MOUSE else "pc",
    "-m", ram,
] + (["-accel", "kvm", "-cpu", "host"]
     if os.environ.get("QEMU_ACCEL") == "kvm" else ["-cpu", "qemu64"]) + [
    "-drive", "if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd",
    "-drive", "if=pflash,format=raw,file=" + work + "/vars.fd",
    "-drive", "if=ide,format=raw,file=" + work + "/uefi.img",
    "-vga", "qxl", "-global", "VGA.vgamem_mb=64",
    "-display", "none", "-serial", "none", "-net", "none",
    "-no-reboot", "-d", "cpu_reset,guest_errors", "-D", work + "/qemu.log",
    "-monitor", "tcp:127.0.0.1:%d,server,nowait" % PORT,
    "-qmp", "tcp:127.0.0.1:%d,server,nowait" % QMP_PORT,
]

# QEMU_EXTRA: extra QEMU arguments, e.g. a display size to offer the guest:
#   QEMU_EXTRA="-global qxl-vga.xres=2560 -global qxl-vga.yres=1080"
cmd += os.environ.get("QEMU_EXTRA", "").split()

# QEMU_USB="kbd,mouse" (or tablet, hub) adds an xHCI controller with those
# devices. Devices listed after "hub" are plugged into the hub.
#
# "disk" is a 64 MiB USB stick: an MBR partition table with one FAT32
# partition holding HELLO.TXT. It is left at /tmp/naq_qemu/stick.img, so
# what the guest wrote can be checked afterwards with
#   mdir -i /tmp/naq_qemu/stick.img@@1M ::
def make_stick(path):
    with open(path, "wb") as f:
        f.truncate(64 * 1024 * 1024)
    subprocess.run(["mkfs.fat", "-F", "32", "--offset", "2048", path],
                   check=True, stdout=subprocess.DEVNULL)
    with open(work + "/hello.txt", "w") as f:
        f.write("hello from the stick\n")
    subprocess.run(["mcopy", "-i", path + "@@1M", work + "/hello.txt",
                    "::HELLO.TXT"], check=True)
    cube = proj + "/build/userapps/CUBE.ELF"
    if os.path.exists(cube):
        subprocess.run(["mcopy", "-i", path + "@@1M", cube, "::CUBE.ELF"],
                       check=True)
    sectors = 64 * 1024 * 1024 // 512
    entry = bytes([0, 0, 0, 0, 0x0C, 0, 0, 0]) + \
        (2048).to_bytes(4, "little") + (sectors - 2048).to_bytes(4, "little")
    with open(path, "r+b") as f:
        f.seek(446)
        f.write(entry)
        f.seek(510)
        f.write(b"\x55\xAA")


usb_devices = [d for d in os.environ.get("QEMU_USB", "").split(",") if d]

# QEMU_BOOT_USB=1 starts the system from a USB stick instead of an IDE disk,
# the way it boots on a real PC.
BOOT_USB = os.environ.get("QEMU_BOOT_USB") == "1"

if usb_devices or BOOT_USB:
    cmd += ["-device", "qemu-xhci,id=xhci"]

if BOOT_USB:
    i = cmd.index("if=ide,format=raw,file=" + work + "/uefi.img")
    cmd[i] = "if=none,id=bootdisk,format=raw,file=" + work + "/uefi.img"
    cmd += ["-device", "usb-storage,bus=xhci.0,drive=bootdisk,bootindex=0"]

if usb_devices:
    hub_port = 0
    for d in usb_devices:
        if d == "disk":
            stick = work + "/stick.img"
            if not (os.environ.get("QEMU_KEEP_DISK") == "1"
                    and os.path.exists(stick)):
                make_stick(stick)
            cmd += ["-drive", "if=none,id=stick,format=raw,file=" + stick]
            d = "storage,drive=stick,id=stickdev"
        if d == "hub":
            cmd += ["-device", "usb-hub,bus=xhci.0,port=1"]
            hub_port = 1
        elif hub_port:
            cmd += ["-device", "usb-%s,bus=xhci.0,port=1.%d" % (d, hub_port)]
            hub_port += 1
        else:
            cmd += ["-device", "usb-%s,bus=xhci.0" % d]
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
        "\n": "ret", ">": "shift-dot", "<": "shift-comma", ",": "comma",
        "=": "equal", "+": "shift-equal", "!": "shift-1", "(": "shift-9",
        ")": "shift-0", '"': "shift-apostrophe", "'": "apostrophe"}


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
        elif op == "wheel":
            # wheel:N turns the wheel N steps (positive = scroll down)
            steps = int(arg)
            name = "wheel-down" if steps > 0 else "wheel-up"
            for _ in range(abs(steps)):
                for down in (True, False):
                    qmp("input-send-event", {"events": [
                        {"type": "btn",
                         "data": {"down": down, "button": name}}]})
                time.sleep(0.05)
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
