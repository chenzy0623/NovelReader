import io
import os
import struct
from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "app.ico")
N = 256


def draw_icon(n):
    S = 8 if n <= 64 else 4          # 小图标用更高倍超采样，保证线条清晰
    W = n * S
    img = Image.new("RGBA", (W, W), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    k = W / 256.0

    def P(*pts):
        return [(x * k, y * k) for (x, y) in pts]

    def R(r):
        return [v * k for v in r]

    def lw(px):
        return max(1, int(round(px * k)))

    # 背景圆角方块（深石板蓝）
    d.rounded_rectangle(R([4, 4, 252, 252]), radius=int(54 * k), fill=(52, 71, 91, 255))

    # 书本左右两页
    left = P((34, 92), (124, 74), (124, 196), (34, 178))
    right = P((132, 74), (222, 92), (222, 178), (132, 196))
    d.polygon(left, fill=(246, 239, 225, 255))
    d.polygon(right, fill=(252, 247, 238, 255))

    # 中缝与外沿
    d.line(P((124, 74), (124, 196)), fill=(198, 182, 156, 255), width=lw(6))
    d.line(P((34, 92), (34, 178)), fill=(214, 201, 178, 255), width=lw(4))
    d.line(P((222, 92), (222, 178)), fill=(214, 201, 178, 255), width=lw(4))

    # 文字行（图标较大时才画，小尺寸保持干净）
    if n >= 32:
        line_col = (178, 160, 130, 255)
        for i in range(5):
            y = 104 + i * 18
            d.line(P((46, y + 7), (110 - i * 2, y + 1)), fill=line_col, width=lw(5))
            d.line(P((146 + i * 2, y + 1), (210, y + 7)), fill=line_col, width=lw(5))

    # 红色书签丝带
    d.polygon(P((176, 58), (206, 58), (206, 134), (191, 118), (176, 134)),
              fill=(192, 57, 43, 255))

    return img.resize((n, n), Image.LANCZOS)


def png_bytes(im):
    b = io.BytesIO()
    im.save(b, format="PNG", optimize=True)
    return b.getvalue()


sizes = [16, 20, 24, 32, 40, 48, 64, 96, 128, 256]
frames = [draw_icon(s) for s in sizes]
blobs = [png_bytes(f) for f in frames]

head = struct.pack("<HHH", 0, 1, len(frames))
off = 6 + 16 * len(frames)
entries = b""
for (im, blob) in zip(frames, blobs):
    w, h = im.size
    entries += struct.pack("<BBBBHHII", w % 256, h % 256, 0, 0, 1, 32, len(blob), off)
    off += len(blob)

with open(OUT, "wb") as f:
    f.write(head + entries + b"".join(blobs))

frames[-1].save(os.path.join(HERE, "icon_256.png"))
# 预览：把各尺寸并排拼一张
strip = Image.new("RGBA", (256 + 8 + 128 + 8 + 64 + 8 + 48 + 8 + 32 + 8 + 24 + 8 + 16, 256), (240, 240, 240, 255))
x = 0
for s in [256, 128, 64, 48, 32, 24, 16]:
    im = frames[sizes.index(s)]
    strip.paste(im, (x, 0), im)
    x += s + 8
strip.save(os.path.join(HERE, "icon_sizes.png"))
print("saved", OUT, os.path.getsize(OUT), "bytes")
