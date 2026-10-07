#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Generates assets/icon0.png (512x512) deterministically using only the standard library.

The design: dark blue gradient tile, a rounded accent frame, and a geometric "A" made of two
strokes and a crossbar. Rendered with 4x4 supersampling for smooth edges.

Usage: python3 tools/generate_icon.py [output.png]
"""

import math
import struct
import sys
import zlib

SIZE = 512
SAMPLES = 4

BG_TOP = (20, 27, 46)
BG_BOTTOM = (8, 11, 20)
ACCENT = (91, 140, 255)
ACCENT_LIGHT = (170, 196, 255)
WHITE = (242, 244, 248)


def inside_rounded_rect(x, y, x0, y0, x1, y1, r):
    if x < x0 or x > x1 or y < y0 or y > y1:
        return False
    cx = min(max(x, x0 + r), x1 - r)
    cy = min(max(y, y0 + r), y1 - r)
    return (x - cx) ** 2 + (y - cy) ** 2 <= r * r


def distance_to_segment(px, py, ax, ay, bx, by):
    dx, dy = bx - ax, by - ay
    t = ((px - ax) * dx + (py - ay) * dy) / (dx * dx + dy * dy)
    t = max(0.0, min(1.0, t))
    qx, qy = ax + t * dx, ay + t * dy
    return math.hypot(px - qx, py - qy)


# Letter geometry (in pixels of the 512 canvas).
APEX = (256, 118)
LEFT_FOOT = (150, 398)
RIGHT_FOOT = (362, 398)
STROKE = 30
BAR_Y = 300


def sample(x, y):
    """Colour of the continuous image at (x, y)."""
    t = y / SIZE
    color = tuple(BG_TOP[i] * (1 - t) + BG_BOTTOM[i] * t for i in range(3))

    # Accent frame.
    outer = inside_rounded_rect(x, y, 36, 36, 476, 476, 72)
    inner = inside_rounded_rect(x, y, 50, 50, 462, 462, 60)
    if outer and not inner:
        color = ACCENT

    # Crossbar first, so the strokes are drawn over its ends.
    if abs(y - BAR_Y) <= 16:
        span = (BAR_Y - APEX[1]) / (LEFT_FOOT[1] - APEX[1])
        xl = APEX[0] + (LEFT_FOOT[0] - APEX[0]) * span
        xr = APEX[0] + (RIGHT_FOOT[0] - APEX[0]) * span
        if xl <= x <= xr:
            color = ACCENT_LIGHT

    # Letter strokes.
    d_left = distance_to_segment(x, y, *APEX, *LEFT_FOOT)
    d_right = distance_to_segment(x, y, *APEX, *RIGHT_FOOT)
    if d_left <= STROKE or d_right <= STROKE:
        # Subtle vertical shading on the strokes.
        shade = 0.85 + 0.15 * (1 - y / SIZE)
        color = tuple(min(255, c * shade) for c in WHITE)
    return color


def render():
    rows = []
    step = 1.0 / SAMPLES
    for py in range(SIZE):
        row = bytearray([0])  # PNG filter type 0
        for px in range(SIZE):
            acc = [0.0, 0.0, 0.0]
            for sy in range(SAMPLES):
                for sx in range(SAMPLES):
                    c = sample(px + (sx + 0.5) * step, py + (sy + 0.5) * step)
                    acc[0] += c[0]
                    acc[1] += c[1]
                    acc[2] += c[2]
            n = SAMPLES * SAMPLES
            row += bytes(int(round(v / n)) for v in acc)
        rows.append(bytes(row))
    return b"".join(rows)


def png(width, height, raw):
    def chunk(kind, data):
        body = kind + data
        return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)

    header = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) + chunk(b"IDAT", zlib.compress(raw, 9)) +
            chunk(b"IEND", b""))


def main():
    output = sys.argv[1] if len(sys.argv) > 1 else "assets/icon0.png"
    with open(output, "wb") as f:
        f.write(png(SIZE, SIZE, render()))
    print("wrote", output)


if __name__ == "__main__":
    main()
