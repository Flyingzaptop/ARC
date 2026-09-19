"""Render actual captured GPU frames to a GIF/contact sheet; no generated imagery."""
import argparse
from pathlib import Path
import numpy as np
from PIL import Image, ImageDraw

parser = argparse.ArgumentParser()
parser.add_argument("directory", type=Path)
args = parser.parse_args()
root = args.directory / "preview"
frames = []
for tick in range(0, 61, 2):
    path = root / f"frame-{tick}.rgb8"
    pixels = np.fromfile(path, dtype=np.uint8)
    if pixels.size != 1920 * 1080 * 3:
        raise ValueError(f"Invalid GPU readback {path}")
    frames.append(Image.fromarray(pixels.reshape(1080, 1920, 3)).resize((960, 540)))
frames[0].save(args.directory / "night-city-flythrough.gif", save_all=True,
               append_images=frames[1:], duration=100, loop=0)
sheet = Image.new("RGB", (1920, 1128), (16, 16, 20))
draw = ImageDraw.Draw(sheet)
for slot, index in enumerate((0, 10, 20, 30)):
    x, y = (slot % 2) * 960, (slot // 2) * 564
    sheet.paste(frames[index], (x, y + 24))
    draw.text((x + 12, y + 6), f"Actual GPU frame: camera tick {index * 2}", fill="white")
sheet.save(args.directory / "night-city-waypoints.png")
print(args.directory / "night-city-flythrough.gif")
