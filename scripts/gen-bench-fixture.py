#!/usr/bin/env python3
"""Generates a synthetic directory of tiny but valid PNG images for the
cold-start benchmark (guide doc, acceptance criterion #1).

Usage: gen-bench-fixture.py <dir> [count]

The names exercise natural sorting (zero-padded) and the content is a real
decodable PNG so the thumbnail pipeline actually runs. Files are ~120 bytes;
100k files occupy ~15 MB.
"""

import os
import struct
import sys
import zlib


def png_bytes(w=16, h=16):
    def chunk(tag, data):
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    ihdr = struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)
    raw = b"".join(b"\x00" + bytes([(i * 7) % 256, 90, 150] * w) for i in range(h))
    return (
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", ihdr)
        + chunk(b"IDAT", zlib.compress(raw))
        + chunk(b"IEND", b"")
    )


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    target = os.path.abspath(sys.argv[1])
    count = int(sys.argv[2]) if len(sys.argv) > 2 else 100_000

    os.makedirs(target, exist_ok=True)
    blob = png_bytes()
    for i in range(count):
        # zero-padded (sorts stably) with a few unpadded stragglers in the
        # first 500 to exercise natural-order comparisons
        name = f"img{i:06d}.png" if i >= 500 else f"img{i}.png"
        with open(os.path.join(target, name), "wb") as f:
            f.write(blob)
        if (i + 1) % 10_000 == 0:
            print(f"{i + 1}/{count}", flush=True)
    print(f"done: {count} files in {target}")


if __name__ == "__main__":
    main()
