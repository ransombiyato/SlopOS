#!/usr/bin/env python3
"""Combine SlopOS screenshots into docs/screenshots.png with labels."""
import os
from PIL import Image, ImageDraw

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SHOTS = [
    ("splash_5.png", "Boot splash"),
    ("boot.png", "Desktop"),
    ("launcher.png", "Launcher"),
    ("files.png", "Files"),
    ("term2.png", "Terminal"),
    ("image.png", "Image Viewer"),
    ("view.png", "File Viewer"),
    ("about.png", "System Info"),
    ("compat.png", "Compat: Zen Browser"),
    ("resolve.png", "Compat: DaVinci Resolve"),
]

CELL_W, CELL_H = 520, 340
COLS = 2
ROWS = (len(SHOTS) + COLS - 1) // COLS
LABEL = 30
PAD = 16

W = COLS * CELL_W + PAD * (COLS + 1)
H = ROWS * (CELL_H + LABEL) + PAD * (ROWS + 1)
canvas = Image.new("RGB", (W, H), (12, 14, 22))
draw = ImageDraw.Draw(canvas)

for i, (name, label) in enumerate(SHOTS):
    path = os.path.join(ROOT, "dist", name)
    if not os.path.exists(path):
        continue
    img = Image.open(path).convert("RGB")
    img.thumbnail((CELL_W, CELL_H))
    cx = PAD + (i % COLS) * (CELL_W + PAD)
    cy = PAD + (i // COLS) * (CELL_H + LABEL + PAD)
    ox = cx + (CELL_W - img.width) // 2
    canvas.paste(img, (ox, cy))
    draw.text((cx + 4, cy + CELL_H + 6), label, fill=(200, 208, 224))

out = os.path.join(ROOT, "docs", "screenshots.png")
os.makedirs(os.path.dirname(out), exist_ok=True)
canvas.save(out)
print(">> wrote", out, canvas.size)
