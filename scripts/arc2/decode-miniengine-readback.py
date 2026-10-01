"""Convert MiniEngine PixelBuffer::ExportToFile R10G10B10A2 GPU readback."""
from __future__ import annotations

import argparse
import hashlib
import struct
from pathlib import Path

import numpy as np
from PIL import Image


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("raw", type=Path)
    parser.add_argument("png", type=Path)
    args = parser.parse_args()
    data = args.raw.read_bytes()
    if len(data) < 16:
        raise ValueError("MiniEngine readback lacks its 16-byte header")
    fmt, pitch, width, height = struct.unpack_from("<IIII", data)
    required = 16 + pitch * (height - 1) + width * 4
    if fmt != 24 or pitch % 4 or pitch < width * 4 or len(data) < required:
        raise ValueError(f"Unexpected GPU readback: format={fmt}, pitch={pitch}, {width}x{height}")
    # MiniEngine omits padding after the last image row.
    padded = data[16:] + bytes(max(0, pitch * height - (len(data) - 16)))
    packed = np.frombuffer(padded, dtype="<u4", count=(pitch // 4) * height)
    packed = packed.reshape(height, pitch // 4)[:, :width]
    rgb = np.stack((packed & 1023, (packed >> 10) & 1023, (packed >> 20) & 1023), axis=2)
    rgb = ((rgb * 255 + 511) // 1023).astype(np.uint8)
    args.png.parent.mkdir(parents=True, exist_ok=True)
    Image.fromarray(rgb, "RGB").save(args.png)
    print(f"GPU readback {width}x{height}, row_pitch={pitch}, raw_sha256={hashlib.sha256(data).hexdigest()}")


if __name__ == "__main__":
    main()
