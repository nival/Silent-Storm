#!/usr/bin/env python3
"""
preview_font.py -- render platform/text_font.cpp's glyph table to a PNG so the
shapes can be checked by eye without deploying to a device.

    python3 tools/preview_font.py [--out /tmp/font.png] [--text "SAMPLE"]
"""
import argparse
import os
import re
import struct
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
FONT_CPP = os.path.join(os.path.dirname(HERE), "platform", "text_font.cpp")

GLYPH_RE = re.compile(r"^\{\s*'((?:\\.|[^'])+)',\s*\{(.*)\}\s*\},\s*$")


def parse_glyphs(path):
    glyphs = {}
    for line in open(path, encoding="utf-8"):
        m = GLYPH_RE.match(line.strip())
        if not m:
            continue
        ch, rows_src = m.groups()
        ch = {"\\'": "'", "\\\\": "\\"}.get(ch, ch)
        rows = re.findall(r'"([^"]*)"', rows_src)
        if len(rows) != 7:
            sys.exit("glyph %r has %d rows, expected 7" % (ch, len(rows)))
        for r in rows:
            if len(r) != 5:
                sys.exit("glyph %r row %r is %d wide, expected 5" % (ch, r, len(r)))
        glyphs[ch] = rows
    return glyphs


def write_png(path, width, height, pixels):
    """pixels: bytearray of RGB triples."""
    raw = b"".join(b"\x00" + bytes(pixels[y * width * 3:(y + 1) * width * 3])
                   for y in range(height))

    def chunk(tag, data):
        c = struct.pack(">I", len(data)) + tag + data
        return c + struct.pack(">I", zlib.crc32(tag + data) & 0xffffffff)

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw, 9))
    png += chunk(b"IEND", b"")
    open(path, "wb").write(png)


def render(glyphs, lines, scale=4):
    cols = max(len(l) for l in lines)
    w, h = cols * 6 * scale, len(lines) * 8 * scale
    px = bytearray(w * h * 3)
    for y in range(h):                      # dark background
        for x in range(w):
            i = (y * w + x) * 3
            px[i:i + 3] = bytes((16, 18, 22))
    for row, line in enumerate(lines):
        for col, ch in enumerate(line):
            rows = glyphs.get(ch)
            if not rows:
                continue
            for gy in range(7):
                for gx in range(5):
                    if rows[gy][gx] != '#':
                        continue
                    for sy in range(scale):
                        for sx in range(scale):
                            x = (col * 6 + gx) * scale + sx
                            y = (row * 8 + gy) * scale + sy
                            i = (y * w + x) * 3
                            px[i:i + 3] = bytes((220, 230, 235))
    return w, h, px


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default="/tmp/silentstorm_font.png")
    ap.add_argument("--text", default=None)
    args = ap.parse_args()

    glyphs = parse_glyphs(FONT_CPP)
    if args.text:
        lines = args.text.split("\\n")
    else:
        lines = [
            "SILENT STORM - ANDROID PORT",
            "abcdefghijklmnopqrstuvwxyz",
            "ABCDEFGHIJKLMNOPQRSTUVWXYZ",
            "0123456789 !\"#$%&'()*+,-./",
            ":;<=>?@[\\]^_`{|}~",
            "Chapters.res: 12 files, 3.4 KB",
        ]
    w, h, px = render(glyphs, lines)
    write_png(args.out, w, h, px)
    print("%d glyphs parsed -> %s (%dx%d)" % (len(glyphs), args.out, w, h))


if __name__ == "__main__":
    main()
