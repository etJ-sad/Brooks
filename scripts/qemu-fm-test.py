#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Drive the file manager in headless QEMU with simulated key presses and
save screenshots after each step.

Usage: qemu-fm-test.py <image> <outdir> [RES]
Needs: qemu-system-x86_64, OVMF, python3-pil (optional, for PNG).
"""
import os, socket, subprocess, sys, time

img, outdir = sys.argv[1], sys.argv[2]
res = sys.argv[3] if len(sys.argv) > 3 else "1920x1080"
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

shot("bootlog")
key("ret", 4)
shot("fm")
key("1", 2); shot("help"); key("esc", 2)
key("down"); key("ret", 2); shot("enter-notes")                # /home/notes (after .., notes is first dir)
key("down"); key("ret", 2)                                      # open todo.txt in viewer
shot("view-todo"); key("esc", 2)
key("backspace", 2)
key("7", 1.5); typ("projects"); key("ret", 2); shot("mkdir")
key("9", 1.5); typ("hello.txt"); key("ret", 2)                 # new file -> editor
typ("Hello from AxonOS"); key("ret"); typ("line two"); shot("editor")
key("f2", 2); key("esc", 2); shot("after-edit")
key("5", 1.5); shot("copy-dialog"); key("ret", 2); shot("after-copy")
key("tab", 1.5); shot("other-panel")
key("8", 1.5); shot("delete-confirm"); key("ret", 2); shot("after-delete")
key("2", 2); key("end", 2); shot("bootlog-viewer"); key("esc", 2)
cmd("quit", 0.5)
q.wait(timeout=10)
print("serial reached FM prompt:", ok)
