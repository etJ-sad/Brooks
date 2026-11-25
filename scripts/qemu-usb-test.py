#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""USB storage test: attach the boot USB drive from the file manager, write
files to it (or, with "verify", only list them after a reboot).

Usage: qemu-usb-test.py <image> <outdir> [write|verify]
Needs: qemu-system-x86_64, OVMF, python3-pil (optional, for PNG).
"""
import os, socket, subprocess, sys, time

img, outdir = sys.argv[1], sys.argv[2]
mode = sys.argv[3] if len(sys.argv) > 3 else "write"
res = "1920x1080"
os.makedirs(outdir, exist_ok=True)
sock = f"/tmp/fmtest-{os.getpid()}.sock"
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

key("ret", 4)
key("u", 15); shot("usb-attach"); key("esc", 2)
key("tab"); key("end")
if mode.startswith("exfat"): key("down")
key("ret", 3); shot("usb-root")
if mode == "exfat-write":
    key("9", 1.5); typ("exfat note.txt"); key("ret", 2)
    typ("written by the Brooks exFAT driver"); key("f2", 3); key("esc", 2)
    key("7", 1.5); typ("New Dir"); key("ret", 3)
    key("end"); key("up"); key("ret", 3)          # enter "New Dir" (dirs sort first; .. then New Dir)
    key("9", 1.5); typ("inner.txt"); key("ret", 2); typ("inner file"); key("f2", 3); key("esc", 2)
    key("backspace", 2); shot("usb-after-write")
elif mode == "write":
    key("9", 1.5); typ("Notes from Brooks.txt"); key("ret", 2)
    typ("written by the Brooks FAT32 driver"); key("f2", 3); key("esc", 2)
    key("7", 1.5); typ("My Folder"); key("ret", 3)
    key("tab")
    key("down"); key("down"); key("5", 1.5); key("ret", 4)   # copy /home/README.txt to the usb panel
    key("tab"); shot("usb0-after-write")
    key("8", 1.5); shot("delete-confirm"); key("esc", 1)
cmd("quit", 0.5)
q.wait(timeout=10)
print("serial reached FM prompt:", ok)
