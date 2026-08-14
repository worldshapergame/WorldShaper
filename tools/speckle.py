# How speckled a render is, as a number — the same number `tools\speckle.ps1` reports.
#
# WHY A SECOND COPY OF A METRIC EXISTS, which is a thing this repository is otherwise against.
#
# `_measure.ps1`'s `Measure-Speckle` is the definition and this is a transliteration of it, line for
# line, because the machine that can now run the renderer headlessly has no PowerShell and no
# System.Drawing (D641). A metric that cannot be computed where the picture is taken is a metric
# nobody takes. The two must not drift: if one changes, change both, and the arithmetic below is
# laid out in the same order as the original so a diff between them is readable.
#
# THE METRIC. For every interior pixel, how far its luminance sits from the MEDIAN of its eight
# neighbours, relative to the local level plus a floor of 8 so a dark room does not read as all
# noise for want of anything to divide by. The median rather than the mean is what makes it a
# speckle metric rather than a sharpness one: a real edge moves the mean of a neighbourhood and
# leaves its median alone, so a facade full of mouldings does not read as grain. Reported as the
# mean over the frame times a thousand, plus the count of outright fireflies — brighter than four
# times their neighbourhood and 24 clear of it — because those are the ones the eye catches and a
# hundred of them in a million pixels move no average at all.
#
#   python3 tools/speckle.py shot.png [more.png ...]
#   python3 tools/speckle.py --top 40 shot.png     # skip the first 40 rows
#
# THE OVERLAY IS IN THE FRAME AND IT IS NOT A CONSTANT. The developer overlay draws over the
# top-left of every screenshot, it defaults on, and there is no flag to turn it off. Text is the
# worst thing a speckle metric can be shown — every glyph edge is a pixel three times its
# neighbours' median — and the DIGITS DIFFER BETWEEN TWO ARMS OF AN A/B, so it is not even a
# constant that cancels. Two arms of one change measured over the whole frame here read 86.66 and
# 102.71; over the picture alone they read what the change actually did. `--top` is how to leave
# it out: pass enough rows to clear the overlay for the resolution being shot.
#
# No third-party modules: PNG is decoded here with zlib alone, because the container this was
# written for has neither numpy nor PIL and a metric with an install step is a metric that stops
# being run.

import struct
import sys
import zlib

# Sampled above this many interior pixels, on one stride for both axes so the samples stay on a
# square lattice and no direction is favoured. At or below it every pixel is examined.
MAX_SAMPLES = 250000


def read_png(path):
    """Returns (width, height, rows) with rows as bytearrays of RGBA, 8 bits a channel."""
    data = open(path, "rb").read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError(f"{path} is not a PNG")
    at = 8
    width = height = depth = colour = 0
    idat = bytearray()
    palette = b""
    while at < len(data):
        (length,) = struct.unpack(">I", data[at : at + 4])
        kind = data[at + 4 : at + 8]
        body = data[at + 8 : at + 8 + length]
        at += 12 + length
        if kind == b"IHDR":
            width, height, depth, colour, _, _, interlace = struct.unpack(">IIBBBBB", body)
            if depth != 8:
                raise ValueError(f"{path}: only 8 bits a channel, not {depth}")
            if interlace != 0:
                raise ValueError(f"{path}: interlaced PNG is not read here")
        elif kind == b"PLTE":
            palette = body
        elif kind == b"IDAT":
            idat += body
        elif kind == b"IEND":
            break

    channels = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[colour]
    raw = zlib.decompress(bytes(idat))
    stride = width * channels
    rows = []
    previous = bytearray(stride)
    at = 0
    for _ in range(height):
        filter_kind = raw[at]
        at += 1
        line = bytearray(raw[at : at + stride])
        at += stride
        # The five PNG filters, undone in place. `left` is the byte one pixel back, which is
        # `channels` bytes and not one.
        if filter_kind == 1:
            for i in range(channels, stride):
                line[i] = (line[i] + line[i - channels]) & 0xFF
        elif filter_kind == 2:
            for i in range(stride):
                line[i] = (line[i] + previous[i]) & 0xFF
        elif filter_kind == 3:
            for i in range(stride):
                left = line[i - channels] if i >= channels else 0
                line[i] = (line[i] + ((left + previous[i]) >> 1)) & 0xFF
        elif filter_kind == 4:
            for i in range(stride):
                left = line[i - channels] if i >= channels else 0
                up = previous[i]
                up_left = previous[i - channels] if i >= channels else 0
                p = left + up - up_left
                pa, pb, pc = abs(p - left), abs(p - up), abs(p - up_left)
                best = left if (pa <= pb and pa <= pc) else (up if pb <= pc else up_left)
                line[i] = (line[i] + best) & 0xFF
        elif filter_kind != 0:
            raise ValueError(f"{path}: unknown row filter {filter_kind}")
        rows.append(line)
        previous = line

    # Everything becomes RGBA so the luminance walk below has one shape to read.
    out = []
    for line in rows:
        if channels == 4:
            out.append(line)
        elif channels == 3:
            wide = bytearray(width * 4)
            for x in range(width):
                wide[x * 4 : x * 4 + 3] = line[x * 3 : x * 3 + 3]
                wide[x * 4 + 3] = 255
            out.append(wide)
        elif channels == 1 and colour == 3:
            wide = bytearray(width * 4)
            for x in range(width):
                i = line[x] * 3
                wide[x * 4 : x * 4 + 3] = palette[i : i + 3]
                wide[x * 4 + 3] = 255
            out.append(wide)
        elif channels == 1:
            wide = bytearray(width * 4)
            for x in range(width):
                wide[x * 4 : x * 4 + 3] = bytes([line[x]]) * 3
                wide[x * 4 + 3] = 255
            out.append(wide)
        else:
            raise ValueError(f"{path}: colour type {colour} is not read here")
    return width, height, out


def measure(path, top=0):
    width, height, rows = read_png(path)
    if width < 3 or height < 3:
        return {"speckle": 0.0, "fireflies": 0, "pixels": 0, "stride": 1}

    lum = [0.0] * (width * height)
    for y in range(height):
        row = rows[y]
        base = y * width
        for x in range(width):
            i = x * 4
            lum[base + x] = 0.2126 * row[i] + 0.7152 * row[i + 1] + 0.0722 * row[i + 2]

    first = max(1, top)
    interior = float((width - 2) * max(0, height - 1 - first))
    stride = 1
    if MAX_SAMPLES > 0 and interior > MAX_SAMPLES:
        stride = int(-(-((interior / MAX_SAMPLES) ** 0.5) // 1))

    total = 0.0
    counted = 0
    fireflies = 0
    for y in range(first, height - 1, stride):
        for x in range(1, width - 1, stride):
            k = y * width + x
            neighbours = [
                lum[(y + dy) * width + (x + dx)]
                for dy in (-1, 0, 1)
                for dx in (-1, 0, 1)
                if not (dx == 0 and dy == 0)
            ]
            neighbours.sort()
            median = (neighbours[3] + neighbours[4]) * 0.5
            total += abs(lum[k] - median) / (median + 8.0)
            counted += 1
            if lum[k] > median * 4.0 + 24.0:
                fireflies += 1

    return {
        "speckle": (1000.0 * total / counted) if counted else 0.0,
        # Scaled back up to the whole frame, because it is a count rather than a mean and a raw
        # count over a sampled subset would fall with resolution for no reason but the sampling.
        "fireflies": fireflies * stride * stride,
        "pixels": counted,
        "stride": stride,
    }


if __name__ == "__main__":
    args = sys.argv[1:]
    top = 0
    if len(args) >= 2 and args[0] == "--top":
        top = int(args[1])
        args = args[2:]
    if not args:
        print("usage: python3 tools/speckle.py [--top N] <png> [png ...]")
        raise SystemExit(2)
    print(f"  {'speckle':>9}  {'fireflies':>10}  {'pixels':>9}  image")
    for path in args:
        m = measure(path, top)
        print(f"  {m['speckle']:9.2f}  {m['fireflies']:10d}  {m['pixels']:9d}  {path}")
