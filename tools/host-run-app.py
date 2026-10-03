#!/usr/bin/env python3
"""Boot an app under a standalone Xvfb (host) and grab the framebuffer.

This mirrors exactly what the SlopOS X11 compatibility bridge does in the
guest (Xvfb -> XWD framebuffer -> compositor window), but on the host, so we
can validate that a real third-party app renders before packing the image.

Usage: host-run-app.py <name> <exe> --env K=V ... -- <args...>
"""
import argparse, os, struct, subprocess, sys, time
from PIL import Image


def read_fb(fbdir):
    path = os.path.join(fbdir, "Xvfb_screen0")
    d = open(path, "rb").read()
    hs = struct.unpack(">I", d[0:4])[0]
    w = struct.unpack(">I", d[16:20])[0]
    h = struct.unpack(">I", d[20:24])[0]
    bpl = struct.unpack(">I", d[48:52])[0]
    pix = d[hs:]
    img = Image.new("RGB", (w, h))
    px = img.load()
    for y in range(h):
        row = y * bpl
        for x in range(w):
            o = row + x * 4
            px[x, y] = (pix[o + 2], pix[o + 1], pix[o])
    return img


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("name")
    ap.add_argument("exe")
    ap.add_argument("--wait", type=float, default=20)
    ap.add_argument("--out", default=None)
    ap.add_argument("--env", action="append", default=[])
    ap.add_argument("args", nargs="*")
    a = ap.parse_args()

    disp = ":9"
    fbdir = "/tmp/hostrun-fb"
    subprocess.run(["rm", "-rf", fbdir])
    os.makedirs(fbdir, exist_ok=True)
    xvfb = subprocess.Popen(
        ["Xvfb", disp, "-screen", "0", "1280x800x24", "-nolisten", "tcp",
         "-fbdir", fbdir], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(2)

    env = dict(os.environ)
    env["DISPLAY"] = disp
    env.setdefault("HOME", "/tmp/hostrun-home")
    for kv in a.env:
        k, _, v = kv.partition("=")
        env[k] = v
    os.makedirs(env["HOME"], exist_ok=True)

    log = open(f"/tmp/hostrun-{a.name}.log", "wb")
    proc = subprocess.Popen([a.exe] + a.args, env=env, stdout=log, stderr=log)
    time.sleep(a.wait)

    try:
        img = read_fb(fbdir)
        out = a.out or f"dist/hostrun-{a.name}.png"
        img.save(out)
        rgb = img.tobytes()
        nonblack = sum(1 for i in range(0, len(rgb), 3)
                       if rgb[i] or rgb[i + 1] or rgb[i + 2])
        print(f">> {a.name}: {img.size} saved {out} nonblack samples {nonblack}")
    except Exception as e:
        print(f"!! could not read framebuffer: {e}")

    proc.terminate()
    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        proc.kill()
    xvfb.terminate()
    log.close()
    print(f">> log: /tmp/hostrun-{a.name}.log")


if __name__ == "__main__":
    main()
