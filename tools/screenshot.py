#!/usr/bin/env python3
"""Boot SlopOS headless in QEMU, optionally send keys, and save a screenshot.

Usage:
    screenshot.py OUT.png [--wait SEC] [--keys ret tab ...] [--cmd "sendkey a"]

Uses the QEMU human monitor over a unix socket so we can inject input and
dump the framebuffer without a display.
"""
import argparse, os, socket, subprocess, sys, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ISO = os.path.join(ROOT, "dist", "slopos.iso")
MON = "/tmp/slopos-mon.sock"
PPM = "/tmp/slopos.ppm"


class Monitor:
    def __init__(self, path, timeout=30):
        deadline = time.time() + timeout
        while time.time() < deadline:
            try:
                self.s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                self.s.connect(path)
                time.sleep(0.3)
                self.s.recv(65536)
                return
            except (FileNotFoundError, ConnectionRefusedError):
                time.sleep(0.2)
        raise RuntimeError("monitor socket never appeared")

    def cmd(self, c, wait=0.4):
        self.s.sendall((c + "\n").encode())
        time.sleep(wait)
        try:
            self.s.recv(65536)
        except Exception:
            pass


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("--wait", type=float, default=16)
    ap.add_argument("--post-wait", type=float, default=0.0)
    ap.add_argument("--keys", nargs="*", default=[])
    ap.add_argument("--cmd", action="append", default=[])
    ap.add_argument("--click", action="append", default=[],
                    help="X,Y to left-click (may be given more than once)")
    ap.add_argument("--mem", default="2048")
    ap.add_argument("--smp", default="2")
    ap.add_argument("--iso", default=ISO)
    ap.add_argument("--uefi", action="store_true", help="boot via OVMF (UEFI)")
    args = ap.parse_args()

    for p in (MON, PPM):
        try:
            os.unlink(p)
        except FileNotFoundError:
            pass

    cmd = [
        "qemu-system-x86_64", "-cdrom", args.iso,
        "-m", args.mem, "-smp", args.smp,
        "-vga", "std", "-display", "none",
        "-serial", "file:/tmp/slopos-serial.log",
        "-monitor", f"unix:{MON},server,nowait",
        "-no-reboot", "-daemonize", "-pidfile", "/tmp/slopos.pid",
    ]
    if args.uefi:
        for code in ("/usr/share/OVMF/OVMF_CODE_4M.fd", "/usr/share/ovmf/OVMF.fd"):
            if os.path.exists(code):
                cmd += ["-drive", f"if=pflash,format=raw,readonly=on,file={code}"]
                break
    print(">> booting", " ".join(cmd))
    subprocess.run(cmd, check=True)

    mon = Monitor(MON)
    print(f">> waiting {args.wait}s for boot")
    time.sleep(args.wait)

    for k in args.keys:
        print(">> sendkey", k)
        mon.cmd(f"sendkey {k}")
    for c in args.cmd:
        print(">>", c)
        mon.cmd(c)
    # QEMU's monitor mouse_move is relative, and the guest cursor starts at
    # the centre of the screen, so track position and send deltas.
    cx, cy = 640, 400
    for c in args.click:
        x, y = (int(v) for v in c.split(","))
        print(f">> click {x},{y}")
        mon.cmd(f"mouse_move {x - cx} {y - cy}", wait=0.2)
        cx, cy = x, y
        mon.cmd("mouse_button 1", wait=0.15)
        mon.cmd("mouse_button 0", wait=0.3)

    if args.post_wait:
        time.sleep(args.post_wait)

    time.sleep(0.8)
    mon.cmd("screendump " + PPM, wait=2.0)

    try:
        with open("/tmp/slopos.pid") as f:
            os.kill(int(f.read().strip()), 9)
    except Exception:
        pass
    time.sleep(0.5)

    from PIL import Image
    img = Image.open(PPM)
    img.save(args.out)
    print(f">> saved {args.out} ({img.width}x{img.height})")

    if os.path.exists("/tmp/slopos-serial.log"):
        with open("/tmp/slopos-serial.log", errors="replace") as f:
            tail = f.read().splitlines()[-25:]
        print("--- serial tail ---")
        print("\n".join(tail))


if __name__ == "__main__":
    main()
