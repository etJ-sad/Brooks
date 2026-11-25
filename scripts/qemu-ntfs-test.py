#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""NTFS test: two NTFS USB disks (GPT partition and whole-disk), attach them
in the file manager, view and copy files to the FAT32 boot drive, compare MD5.

Usage: qemu-ntfs-test.py <image> <outdir>
Needs: qemu-system-x86_64, OVMF, mtools, ntfs-3g (FUSE + sudo to build the test volumes).
"""
import os, socket, subprocess, sys, time
import hashlib, shutil

img, outdir = sys.argv[1], os.path.abspath(sys.argv[2])
os.makedirs(outdir, exist_ok=True)
work = os.path.join(outdir, "vol")
os.makedirs(work, exist_ok=True)
mnt = os.path.join(work, "mnt")
os.makedirs(mnt, exist_ok=True)

def sh(*a, **kw):
    subprocess.run(list(a), check=True, **kw)

def md5(b):
    return hashlib.md5(b).hexdigest()

def build_volume(path, size_mb, label, fill):
    with open(path, "wb") as f:
        f.truncate(size_mb << 20)
    sh("mkntfs", "-Q", "-F", "-L", label, path, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    sh("sudo", "ntfs-3g", path, mnt)
    try:
        sh("sudo", "python3", "-I", "-c", fill, mnt)
    finally:
        sh("sudo", "umount", mnt)

# Volume 1 (GPT partition): Cyrillic names, nested folders, a 400-entry
# directory (index allocation), a resident file, a sparse file.
FILL1 = r'''
import os, sys, random
m = sys.argv[1]
os.makedirs(m + "/Документы/Проекты"); os.makedirs(m + "/big")
open(m + "/Документы/Привет.txt", "w").write("Привет из NTFS! Hello from NTFS.\n")
open(m + "/Документы/Проекты/план.txt", "w").write("nested file\n")
for i in range(1, 401): open(m + f"/big/file_{i}.txt", "w").write(f"file {i}\n")
open(m + "/tiny.txt", "w").write("small resident\n")
random.seed(1)
open(m + "/random.bin", "wb").write(random.randbytes(3 << 20))
with open(m + "/sparse.bin", "wb") as f:
    f.truncate(10 << 20); f.seek((10 << 20) - 3); f.write(b"END")
'''
# Volume 2 (whole disk, no partition table): fill it with 8 KiB files, free
# every other one, then write a file that has to go into the holes
# (fragmented data runs); the 2000+ entry directory needs a multi-level index.
FILL2 = r'''
import os, sys
m = sys.argv[1]
os.makedirs(m + "/h")
i = 0
try:
    while True:
        with open(f"{m}/h/{i}", "wb") as f: f.write(os.urandom(8192))
        i += 1
except OSError:
    pass
os.sync()
for k in range(0, i, 2): os.truncate(f"{m}/h/{k}", 0)
os.sync()
open(m + "/frag.bin", "wb").write(os.urandom(2 << 20))
'''
part = os.path.join(work, "part.img")
disk1 = os.path.join(work, "ntfs-gpt.img")
disk2 = os.path.join(work, "ntfs-whole.img")
build_volume(part, 254, "BROOKSNTFS", FILL1)
with open(disk1, "wb") as f:
    f.truncate(256 << 20)
sh("sgdisk", "--clear", "--new=1:2048:0", "--typecode=1:0700", disk1, stdout=subprocess.DEVNULL)
sh("dd", f"if={part}", f"of={disk1}", "bs=1M", "seek=1", "conv=notrunc", stderr=subprocess.DEVNULL)
build_volume(disk2, 24, "SMALL", FILL2)
ref = {}
sh("sudo", "ntfs-3g", "-o", "ro", part, mnt)
for n in ("random.bin", "sparse.bin"):
    ref[n] = md5(open(os.path.join(mnt, n), "rb").read())
sh("sudo", "umount", mnt)
sh("sudo", "ntfs-3g", "-o", "ro", disk2, mnt)
ref["frag.bin"] = md5(open(os.path.join(mnt, "frag.bin"), "rb").read())
sh("sudo", "umount", mnt)
os.remove(part)

boot = os.path.join(work, "boot.img")
shutil.copy(img, boot)
sock = f"/tmp/ntfstest-{os.getpid()}.sock"
log = open(os.path.join(outdir, "serial.log"), "wb")
extra = (f"-drive if=none,id=n1,format=raw,file={disk1} -device usb-storage,bus=xhci.0,drive=n1 "
         f"-drive if=none,id=n2,format=raw,file={disk2} -device usb-storage,bus=xhci.0,drive=n2")
env = dict(os.environ, HEADLESS="1", RES="1920x1080", QEMU_EXTRA=extra)
root = os.path.dirname(os.path.abspath(__file__))
q = subprocess.Popen([os.path.join(root, "run-qemu.sh"), boot, "-monitor", f"unix:{sock},server,nowait"],
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

step = [0]
def shot(name):
    step[0] += 1
    path = os.path.join(outdir, f"{step[0]:02d}-{name}.ppm")
    cmd(f"screendump {path}", 2.5)
    try:
        from PIL import Image
        Image.open(path).save(path[:-4] + ".png"); os.remove(path)
    except Exception:
        pass

# Mounts: /usb0 = boot FAT32, /usb1 = NTFS (GPT), /usb2 = NTFS (whole disk).
key("ret", 4)
key("u", 20); shot("attach"); key("esc", 2)
key("backspace", 2); key("end"); key("up"); key("up"); key("ret", 3)   # left panel: /usb0
key("tab"); key("end"); key("ret", 4); shot("usb2-root")               # right panel: /usb2
key("end"); key("5", 1.5); key("ret", 20)                                # copy frag.bin -> /usb0
key("home"); key("down"); key("ret", 15); shot("usb2-h-2399")          # big multi-level index
key("backspace", 3); key("backspace", 2)
key("end"); key("up"); key("ret", 4); shot("usb1-root")                # /usb1
# /usb1: .., big, Документы, random.bin, sparse.bin, tiny.txt
key("end"); key("ret", 3); shot("view-tiny"); key("esc", 2)
key("up"); key("5", 1.5); key("ret", 40)                                 # sparse.bin
key("up"); key("5", 1.5); key("ret", 20)                                 # random.bin
key("home"); key("down"); key("down"); key("ret", 3)                    # Документы
key("end"); key("ret", 3); shot("view-privet"); key("esc", 2)
key("home"); key("down"); key("ret", 3); shot("projects")
cmd("quit", 0.5)
q.wait(timeout=20)

# Compare what the kernel copied onto the FAT32 boot drive.
good = True
for n, h in ref.items():
    out = os.path.join(work, "got-" + n)
    r = subprocess.run(["mcopy", "-o", "-i", boot + "@@1M", "::/" + n, out], capture_output=True)
    got = md5(open(out, "rb").read()) if r.returncode == 0 else "missing"
    print(f"{n}: {'OK' if got == h else 'MISMATCH'} ({got})")
    good &= got == h
print("serial reached FM prompt:", ok)
print("NTFS TEST:", "OK" if good and ok else "FAILED")
sys.exit(0 if good and ok else 1)
