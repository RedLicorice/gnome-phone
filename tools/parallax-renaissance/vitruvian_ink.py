#!/usr/bin/python3 -I
# vitruvian_ink.py SCAN.jpg OUT.png: the ink mask (white = ink) of Leonardo's Vitruvian Man, from the public-domain scan
# (Wikimedia Commons "Da_Vinci_Vitruve_Luc_Viatour.jpg", 2258x3070); build_renaissance.py reads it as vitruvian_ink.png.
# The ink is what is darker than the paper around it (paper estimated by a wide blur), then a soft knee.
import sys
import numpy as np
from PIL import Image, ImageFilter
src, out = sys.argv[1], sys.argv[2]
im = Image.open(src).convert("RGB").crop((330, 640, 1970, 2250))     # the square with the figure
g = np.asarray(im.convert("L"), np.float32)
bg = np.asarray(Image.fromarray(g.astype(np.uint8)).filter(ImageFilter.GaussianBlur(40)), np.float32)
bg = np.maximum(bg, 1)
ink = np.clip((bg - g) / bg, 0, 1)
ink = np.clip((ink - 0.12) / 0.28, 0, 1) ** 0.8
Image.fromarray((ink * 255).astype(np.uint8)).filter(ImageFilter.MedianFilter(3)).save(out)
