#!/usr/bin/env python3
"""Resolve and fetch Ubuntu (noble) packages into a staging tree.

The SlopOS image is Debian-based, but the prebuilt OBS Studio release targets
Ubuntu 24.04 (noble) and needs noble-era libraries (Qt6, FFmpeg 6, mbedTLS
2.28, ...) whose sonames do not exist in Debian 13.

This resolves the full dependency closure from the noble package indexes,
downloads each .deb, and extracts it into <out-root>. Libraries land on the
standard search path (<out-root>/usr/lib/x86_64-linux-gnu) so the compatibility
runtime finds them next to the app.

Usage:
    fetch-ubuntu-deb.py <out-root> <package> [package ...]
"""
import gzip
import os
import re
import shutil
import subprocess
import sys
import urllib.request

MIRROR = "https://archive.ubuntu.com/ubuntu"
SUITES = ["noble", "noble-updates"]
COMPONENTS = ["main", "universe"]
IDX = "/tmp/slopos-noble-index"
DEBS = "/tmp/slopos-noble-deb"
EXTRACT = "/tmp/slopos-noble-extract"

# Debian already provides these; keep Debian's (usually newer) copies instead
# of overlaying Ubuntu's, so the base system stays consistent.
HOST_LIBDIRS = ["/usr/lib/x86_64-linux-gnu", "/lib/x86_64-linux-gnu"]
HOST_LIBS = set()
for d in HOST_LIBDIRS:
    if os.path.isdir(d):
        for n in os.listdir(d):
            HOST_LIBS.add(n)


def ensure_index(suite, comp):
    os.makedirs(IDX, exist_ok=True)
    p = os.path.join(IDX, f"{suite}-{comp}.gz")
    if os.path.exists(p) and os.path.getsize(p) > 1000:
        return p
    url = f"{MIRROR}/dists/{suite}/{comp}/binary-amd64/Packages.gz"
    print(f">> fetching index {url}")
    urllib.request.urlretrieve(url, p)
    return p


def parse(path):
    pkgs, cur = {}, {}
    with gzip.open(path, "rt", errors="replace") as f:
        for line in f:
            line = line.rstrip("\n")
            if not line:
                if "Package" in cur and "Filename" in cur:
                    old = pkgs.get(cur["Package"])
                    if not old or cur.get("Version", "") > old.get("Version", ""):
                        pkgs[cur["Package"]] = cur
                cur = {}
                continue
            if line[0] in " \t" or ":" not in line:
                continue
            k, v = line.split(":", 1)
            cur[k.strip()] = v.strip()
    return pkgs


def load_index():
    all_pkgs = {}
    for suite in SUITES:
        for comp in COMPONENTS:
            try:
                all_pkgs.update(parse(ensure_index(suite, comp)))
            except Exception as e:
                print(f"!! index {suite}/{comp}: {e}")
    return all_pkgs


def dep_names(stanza, real):
    for field in ("Pre-Depends", "Depends"):
        for clause in stanza.get(field, "").split(","):
            clause = clause.strip()
            if not clause:
                continue
            for alt in clause.split("|"):
                name = re.sub(r"\s*\(.*?\)", "", alt).strip()
                name = name.split("[")[0].strip()
                if name in real:
                    yield name
                    break


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    root, seeds = sys.argv[1], sys.argv[2:]
    pkgs = load_index()
    print(f">> {len(pkgs)} packages in the noble index")

    os.makedirs(os.path.join(root, "usr/lib/x86_64-linux-gnu"), exist_ok=True)
    os.makedirs(DEBS, exist_ok=True)
    os.makedirs(EXTRACT, exist_ok=True)

    seen, queue, missing = set(), list(seeds), []
    while queue:
        name = queue.pop()
        if name in seen:
            continue
        seen.add(name)
        st = pkgs.get(name)
        if not st:
            missing.append(name)
            continue
        queue.extend(dep_names(st, pkgs))

    print(f">> dependency closure: {len(seen)} packages")

    def libname(path):
        b = os.path.basename(path)
        m = re.match(r"(lib[^.]+\.so\.[0-9.]+)", b)
        return m.group(1) if m else b

    staged = set()
    for name in sorted(seen):
        st = pkgs.get(name)
        if not st:
            continue
        deb = os.path.join(DEBS, os.path.basename(st["Filename"]))
        if not os.path.exists(deb):
            url = f"{MIRROR}/{st['Filename']}"
            try:
                urllib.request.urlretrieve(url, deb)
            except Exception as e:
                print(f"!! download {name}: {e}")
                continue
        tmp = os.path.join(EXTRACT, os.path.basename(deb))
        shutil.rmtree(tmp, ignore_errors=True)
        subprocess.run(["dpkg-deb", "-x", deb, tmp], check=True)
        # Qt ships its platform plugins (libqxcb.so and friends) in a qt6/
        # subtree; copy the whole tree so QPA can find them at runtime.
        qt6 = os.path.join(tmp, "usr/lib/x86_64-linux-gnu/qt6")
        if os.path.isdir(qt6):
            shutil.copytree(qt6,
                            os.path.join(root, "usr/lib/x86_64-linux-gnu/qt6"),
                            dirs_exist_ok=True)
        for libdir in ("usr/lib/x86_64-linux-gnu", "lib/x86_64-linux-gnu"):
            src = os.path.join(tmp, libdir)
            if not os.path.isdir(src):
                continue
            for dirpath, _dirs, files in os.walk(src):
                for entry in files:
                    full = os.path.join(dirpath, entry)
                    if not os.path.isfile(full) and not os.path.islink(full):
                        continue
                    rel = os.path.relpath(full, src)
                    if rel.startswith("qt6" + os.sep):
                        continue        # handled above, keep the tree intact
                    if libname(entry) in HOST_LIBS:
                        continue        # Debian already has this soname
                    dst = os.path.join(root, "usr/lib/x86_64-linux-gnu", rel)
                    os.makedirs(os.path.dirname(dst), exist_ok=True)
                    if os.path.exists(dst):
                        continue
                    shutil.copy2(os.path.realpath(full), dst)
                    os.chmod(dst, 0o755)
                    staged.add(entry)
    print(f">> staged {len(staged)} noble libraries, skipped {len(HOST_LIBS)} host sonames")
    if missing:
        print("!! not found in noble: " + ", ".join(sorted(set(missing))))
    print(f">> extracted into {root}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
