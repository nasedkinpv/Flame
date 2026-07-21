#!/usr/bin/env python3
"""Quarantine composite texture-cache pages from the HD set.

DK2 packs sprites into shared 128x128 cache pages and updates them partially,
so some dumped pages are collages (a sprite in the corner of a ground tile).
Their HD variants cause visible inconsistency and should not be replacements.

Detected by content, no dependencies (pure-python PNG reader):
  A. sprite colorkey pixels (saturated cyan/magenta/green clusters)
  B. full-length straight seams exactly on the 32/64/96 px grid
  C. texture ids with multiple hashes in index.csv (partial page updates)

Matching HD files are MOVED to <hd>/_review, never deleted.

Usage:
  python3 tools/curate_textures.py \
      --dump "$HOME/Library/Application Support/Dungeon Keeper II/texture-dump" \
      --hd   "$HOME/Library/Application Support/Dungeon Keeper II/textures-hd"
"""
import argparse
import collections
import csv
import pathlib
import re
import shutil
import struct
import sys
import zlib


def read_png(path):
    """Minimal reader for our own dumps: 8-bit RGB/RGBA, non-interlaced."""
    data = path.read_bytes()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        return None
    pos, width, height, channels, idat = 8, 0, 0, 0, b""
    while pos < len(data):
        length, kind = struct.unpack_from(">I4s", data, pos)
        chunk = data[pos + 8:pos + 8 + length]
        pos += 12 + length
        if kind == b"IHDR":
            width, height, depth, color = struct.unpack_from(">IIBB", chunk)
            if depth != 8 or color not in (2, 6):
                return None
            channels = 3 if color == 2 else 4
        elif kind == b"IDAT":
            idat += chunk
        elif kind == b"IEND":
            break
    raw = zlib.decompress(idat)
    stride = width * channels
    out = bytearray(width * height * channels)
    prev = bytearray(stride)
    pos = 0
    for y in range(height):
        filt = raw[pos]
        line = bytearray(raw[pos + 1:pos + 1 + stride])
        pos += 1 + stride
        if filt == 1:
            for i in range(channels, stride):
                line[i] = (line[i] + line[i - channels]) & 0xFF
        elif filt == 2:
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 0xFF
        elif filt == 3:
            for i in range(stride):
                left = line[i - channels] if i >= channels else 0
                line[i] = (line[i] + ((left + prev[i]) >> 1)) & 0xFF
        elif filt == 4:
            for i in range(stride):
                a = line[i - channels] if i >= channels else 0
                b = prev[i]
                c = prev[i - channels] if i >= channels else 0
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pred = a if pa <= pb and pa <= pc else b if pb <= pc else c
                line[i] = (line[i] + pred) & 0xFF
        out[y * stride:(y + 1) * stride] = line
        prev = line
    return width, height, channels, bytes(out)


def colorkey_pixels(width, height, channels, pixels):
    count = 0
    for i in range(0, width * height * channels, channels):
        r, g, b = pixels[i], pixels[i + 1], pixels[i + 2]
        if (r < 60 and g > 200 and b > 200) or \
           (r > 200 and g < 60 and b > 200) or \
           (r < 60 and g > 220 and b < 60):
            count += 1
    return count


def seam_strength(width, height, channels, pixels, x):
    """Mean |left-right| luma difference across a full-height vertical seam."""
    stride = width * channels
    total = 0
    for y in range(height):
        o = y * stride + x * channels
        left = pixels[o - channels] + pixels[o - channels + 1] + pixels[o - channels + 2]
        right = pixels[o] + pixels[o + 1] + pixels[o + 2]
        total += abs(left - right)
    return total / (3 * height)


def grid_seam(width, height, channels, pixels):
    if width < 128 or height < 128:
        return False
    baseline = sum(seam_strength(width, height, channels, pixels, x)
                   for x in (13, 29, 45, 77, 93, 109)) / 6
    for x in (32, 64, 96):
        s = seam_strength(width, height, channels, pixels, x)
        if s > 18 and s > 4 * (baseline + 1):
            return True
    # transpose check for horizontal seams
    stride = width * channels
    for y in (32, 64, 96):
        total = 0
        for x in range(width):
            o = y * stride + x * channels
            up = pixels[o - stride] + pixels[o - stride + 1] + pixels[o - stride + 2]
            down = pixels[o] + pixels[o + 1] + pixels[o + 2]
            total += abs(up - down)
        s = total / (3 * width)
        if s > 18 and s > 4 * (baseline + 1):
            return True
    return False


def block_devs(width, height, channels, pixels):
    """Per-32x32-block mean-color deviation from the page median."""
    stride = width * channels
    means = []
    for by in range(0, height - 31, 32):
        for bx in range(0, width - 31, 32):
            r = g = b = 0
            for y in range(by, by + 32):
                o = y * stride + bx * channels
                for x in range(32):
                    r += pixels[o]
                    g += pixels[o + 1]
                    b += pixels[o + 2]
                    o += channels
            n = 32 * 32
            means.append((r / n, g / n, b / n))
    med = [sorted(m[i] for m in means)[len(means) // 2] for i in range(3)]
    return [sum(abs(m[i] - med[i]) for i in range(3)) / 3 for m in means]


def isolated_blocks(width, height, channels, pixels):
    """A few blocks alien to an otherwise homogeneous page = collage corner."""
    if width < 64 or height < 64:
        return False
    devs = block_devs(width, height, channels, pixels)
    outliers = [d for d in devs if d > 28]
    rest = sorted(d for d in devs if d <= 28)
    return 1 <= len(outliers) <= 6 and bool(rest) and rest[len(rest) // 2] < 10


def fire_on_black(width, height, channels, pixels):
    """Torch/heart cache pages: a 32x32 tile that is both mostly black and
    carries a warm-pixel cluster (fire baked on black)."""
    stride = width * channels
    for by in range(0, height - 31, 32):
        for bx in range(0, width - 31, 32):
            fire = black = 0
            for y in range(by, by + 32):
                o = y * stride + bx * channels
                for _ in range(32):
                    r, g, b = pixels[o + 2], pixels[o + 1], pixels[o]
                    if r > 150 and g > 60 and b < 110 and r > b + 80:
                        fire += 1
                    if r + g + b < 75:
                        black += 1
                    o += channels
            if fire > 25 and black > 150:
                return True
    return False


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dump", required=True, type=pathlib.Path)
    ap.add_argument("--hd", required=True, type=pathlib.Path)
    ap.add_argument("--apply", action="store_true",
                    help="move flagged HD files to _review (default: report only)")
    args = ap.parse_args()
    review = args.hd / "_review"

    multi = set()
    index = args.dump / "index.csv"
    if index.exists():
        by_id = collections.defaultdict(set)
        for row in csv.reader(open(index)):
            if len(row) >= 2:
                by_id[row[1]].add(row[0])
        multi = {h for hs in by_id.values() if len(hs) > 1 for h in hs}

    name_re = re.compile(r"^([0-9a-f]{16})_\d+x\d+\.png$")
    flagged = {}
    for path in sorted(args.dump.iterdir()):
        m = name_re.match(path.name)
        if not m:
            continue
        digest = m.group(1)
        reasons = []
        if digest in multi:
            reasons.append("multi-hash")
        try:
            png = read_png(path)
        except Exception:
            png = None
        if png:
            if colorkey_pixels(*png) > 8:
                reasons.append("colorkey")
            if grid_seam(*png):
                reasons.append("grid-seam")
            if isolated_blocks(*png):
                reasons.append("alien-block")
            if fire_on_black(*png):
                reasons.append("fire-collage")
        if reasons:
            flagged[digest] = reasons

    moved = 0
    for digest, reasons in sorted(flagged.items()):
        hd_file = args.hd / f"{digest}.png"
        state = "-"
        if hd_file.exists():
            if args.apply:
                review.mkdir(parents=True, exist_ok=True)
                shutil.move(hd_file, review / hd_file.name)
                state = "moved"
                moved += 1
            else:
                state = "would move"
        print(f"{digest}  {','.join(reasons):<24} {state}")
    print(f"\nflagged: {len(flagged)}, hd {'moved' if args.apply else 'to move'}: "
          f"{moved if args.apply else sum(1 for d in flagged if (args.hd / (d + '.png')).exists())}")


if __name__ == "__main__":
    main()
