#!/usr/bin/env python3
"""Stage the shared libraries a set of third-party binaries needs.

SlopOS native apps are static, but the compatibility world (Xvfb plus the
apps that run on it) is ordinary dynamically linked ELF. Given a destination
rootfs and a list of binaries, this resolves each one's dependencies with
`ldd`, follows them transitively, and copies every library into
<root>/lib/x86_64-linux-gnu plus the ELF interpreter into <root>/lib64.

Usage: stage-dynlibs.py <rootfs> <binary> [binary ...]
"""
import os, re, shutil, subprocess, sys

ROOT = sys.argv[1]
BINS = sys.argv[2:]
LIBDIR = os.path.join(ROOT, "lib/x86_64-linux-gnu")
os.makedirs(LIBDIR, exist_ok=True)
os.makedirs(os.path.join(ROOT, "lib64"), exist_ok=True)

seen = set()
queue = list(BINS)
copied = 0


def deps(path):
    try:
        out = subprocess.run(["ldd", path], capture_output=True, text=True,
                             timeout=30).stdout
    except Exception:
        return []
    found = []
    for line in out.splitlines():
        m = re.search(r"=>\s+(\S+)\s+\(", line)
        if m:
            found.append(m.group(1))
        elif re.search(r"^\s*/\S+\s+\(", line):
            found.append(line.split()[0])
    return found


while queue:
    b = queue.pop()
    if b in seen or not os.path.exists(b):
        continue
    seen.add(b)
    for d in deps(b):
        if not os.path.exists(d) or d in seen:
            continue
        if "/ld-linux" in d or re.search(r"/ld-[0-9.]*\.so", d):
            dst = os.path.join(ROOT, "lib64/" + os.path.basename(d))
            shutil.copy2(d, dst)
            os.chmod(dst, 0o755)
            seen.add(d)
            continue
        dst = os.path.join(LIBDIR, os.path.basename(d))
        if not os.path.exists(dst):
            shutil.copy2(d, dst)
            os.chmod(dst, 0o755)
            copied += 1
        queue.append(d)

print(f">> staged {copied} shared libraries for {len(BINS)} binaries")
