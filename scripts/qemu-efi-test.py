#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""UEFI runtime test: open the /sys/efi files in the file manager, then power
off through the F10 menu (UEFI ResetSystem) and check that QEMU exits.

Usage: qemu-efi-test.py <image> <outdir> [RES]
Needs: qemu-system-x86_64, OVMF, python3-pil (optional, for PNG).
"""
import os, socket, subprocess, sys, time

img, outdir = sys.argv[1], sys.argv[2]
res = sys.argv[3] if len(sys.argv) > 3 else "1920x1080"
os.makedirs(outdir, exist_ok=True)
sock = f"/tmp/efitest-{os.getpid()}.sock"
log = open(os.path.join(outdir, "serial.log"), "wb")
env = dict(os.environ, HEADLESS="1", RES=res)
root = os.path.dirname(os.path.abspath(__file__))
q = subprocess.Popen([os.path.join(root, "run-qemu.sh"), img, "-monitor", f"unix:{sock},server,nowait"],
                     stdout=log, stderr=subprocess.STDOUT, env=env)

def wait_serial(text, timeout=180):
    t0 = time.time()
    while time.time() - t0 < timeout:
        with open(os.path.join(outdir, "serial.log"), "rb") as f:
            if text.encode() in f.read():
                return True
        time.sleep(0.5)
    return False

ok = wait_serial("Enter:")
time.sleep(1)
m = socket.socket(socket.AF_UNIX)
m.connect(sock)
time.sleep(0.3)
m.recv(65536)

def cmd(c, pause=0.4):
    m.send((c + "\n").encode())
    time.sleep(pause)
    try:
        m.setblocking(False); m.recv(65536)
    except BlockingIOError:
        pass
    m.setblocking(True)

def key(k, pause=0.6):
    cmd(f"sendkey {k}", pause)

def typ(s):
    special = {" ": "spc", ".": "dot", "/": "slash", "-": "minus", "_": "shift-minus"}
    for ch in s:
        if ch in special: key(special[ch], 0.25)
        elif ch.isupper(): key("shift-" + ch.lower(), 0.25)
        else: key(ch, 0.25)

step = [0]
def shot(name):
    step[0] += 1
    path = os.path.abspath(os.path.join(outdir, f"{step[0]:02d}-{name}.ppm"))
    cmd(f"screendump {path}", 2.5)
    try:
        from PIL import Image
        Image.open(path).save(path[:-4] + ".png"); os.remove(path)
    except Exception:
        pass


key("ret", 4)                       # file manager, starts in /home
key("backspace", 2); shot("root")
key("end", 1); key("ret", 2); shot("sys")
key("home", 1); key("down", 1); key("ret", 2); shot("sys-efi")
for i, name in enumerate(["boot", "info", "variables"]):
    key("down", 1); key("ret", 2); shot(f"view-{name}"); key("esc", 2)
key("f10", 2); shot("power-menu"); key("p", 1)
try:
    q.wait(timeout=30)
    print("powered off: yes (QEMU exited)")
except subprocess.TimeoutExpired:
    print("powered off: NO"); q.kill()
print("serial reached FM prompt:", ok)
