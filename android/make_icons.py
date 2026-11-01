import os
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
src = os.path.join(HERE, "..", "res", "app.ico")

# ICO 里有多帧；用 Pillow 逐帧试，取最大的那一帧（通常是 256x256）
frames = []
with open(src, "rb") as f:
    head = f.read(6)
count = int.from_bytes(head[4:6], "little")
for i in range(count):
    try:
        img = Image.open(src)
        img.size = img.ico.sizes()[i] if hasattr(img, "ico") else img.size
        img.load()
        frames.append(img.convert("RGBA"))
    except Exception:
        pass
img = Image.open(src)
img.size = max(img.ico.sizes()) if hasattr(img, "ico") else img.size
img.load()
base = img.convert("RGBA")
print("base size", base.size)

out = os.path.join(HERE, "res")
for folder, px in {"mipmap-mdpi":48,"mipmap-hdpi":72,"mipmap-xhdpi":96,"mipmap-xxhdpi":144,"mipmap-xxxhdpi":192}.items():
    d = os.path.join(out, folder)
    os.makedirs(d, exist_ok=True)
    base.resize((px, px), Image.LANCZOS).save(os.path.join(d, "ic_launcher.png"), "PNG", optimize=True)
    print("wrote", folder, px)