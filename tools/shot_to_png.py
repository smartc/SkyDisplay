"""Convert a SkyDisplay serial screenshot (raw big-endian RGB565 after a 'SHOT w h' line) to a 2x PNG."""
import sys
from PIL import Image

raw, out = sys.argv[1], sys.argv[2]
b = open(raw, "rb").read()
i = b.find(b"SHOT ")
if i < 0:
    sys.exit("no screenshot header found")
nl = b.find(b"\n", i)
w, h = map(int, b[i:nl].split()[1:3])
px = b[nl + 1:nl + 1 + w * h * 2]
if len(px) < w * h * 2:
    sys.exit(f"short screenshot: {len(px)} of {w * h * 2} bytes")
img = Image.new("RGB", (w, h))
img.putdata([(((v >> 11) & 31) * 255 // 31, ((v >> 5) & 63) * 255 // 63, (v & 31) * 255 // 31)
             for v in ((px[k] << 8) | px[k + 1] for k in range(0, len(px), 2))])
img.resize((w * 2, h * 2), Image.NEAREST).save(out)
print("saved", out)
