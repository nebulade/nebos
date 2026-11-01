#!/usr/bin/env python3
"""Generate the image-library host test corpus (development only).

The generated images and expected hashes are committed; `make test-images`
does not need PIL.  Run this script after changing the decoder or corpus.
"""
import gzip
import os
import struct
import zlib
from PIL import Image, ImageDraw

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "testdata", "images")

def fnv(im, alpha=False):
    h = 2166136261
    for r, g, b, a in im.convert("RGBA").getdata():
        # RGB under a fully transparent pixel is not meaningful.
        if not alpha or a != 0:
            px = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
            h = ((h ^ (px & 255)) * 16777619) & 0xffffffff
            h = ((h ^ ((px >> 8) & 255)) * 16777619) & 0xffffffff
        if alpha:
            h = ((h ^ a) * 16777619) & 0xffffffff
    return h

def main():
    os.makedirs(os.path.join(OUT, "ref"), exist_ok=True)
    w, h = 97, 61
    rgb = Image.new("RGB", (w, h))
    p = rgb.load()
    for y in range(h):
        for x in range(w):
            p[x, y] = ((x * 3 + y * 7) % 256, (x * 5) % 256, (y * 11) % 256)
    rgba = rgb.convert("RGBA")
    q = rgba.load()
    for y in range(h):
        for x in range(w):
            q[x, y] = (q[x, y][0], q[x, y][1], q[x, y][2], (x * 9) % 256)
    gray = rgb.convert("L")
    bw = rgb.convert("1")
    pal = Image.new("P", (w, h))
    pal.putpalette([i % 256 for i in range(768)])
    pp = pal.load()
    for y in range(h):
        for x in range(w):
            pp[x, y] = (x + y) % 16
    pal16 = pal.copy()
    pal_inter = pal.copy()
    rgb_inter = rgb.copy()
    rgba_inter = rgba.copy()
    gray_inter = gray.copy()

    files = {}
    def save(im, name, alpha=False, **kw):
        im.save(os.path.join(OUT, name), **kw)
        files[name] = (im, alpha)

    save(rgb, "rgb.png")
    save(rgba, "rgba.png", True)
    save(gray, "gray.png")
    save(bw, "bw.png")
    save(pal, "pal.png", True, transparency=5)
    files["pal.png"] = (Image.open(os.path.join(OUT, "pal.png")).convert("RGBA"), True)
    save(pal16, "pal4.png")
    save(rgb_inter, "rgb_inter.png", interlace=1)
    save(rgba_inter, "rgba_inter.png", True, interlace=1)
    save(gray_inter, "gray_inter.png", interlace=1)
    # 16-bit grayscale: values are multiples of 257, so high byte is the
    # 8-bit gradient.  Expected hash comes from the 8-bit image.
    g16 = Image.new("I;16", (w, h))
    g8 = Image.new("L", (w, h))
    gg = g16.load()
    g8p = g8.load()
    for y in range(h):
        for x in range(w):
            v = (x * 3 + y * 7) % 256
            gg[x, y] = v * 257
            g8p[x, y] = v
    g16.save(os.path.join(OUT, "gray16.png"))
    files["gray16.png"] = (g8, False)

    save(rgb, "rgb.bmp")
    if os.path.exists(os.path.join(OUT, "rle8.bmp")):
        files["rle8.bmp"] = (Image.open(os.path.join(OUT, "rle8.bmp")).convert("RGB"), False)
    # Top-down BMP: reverse the stored rows and make the DIB height negative.
    src = open(os.path.join(OUT, "rgb.bmp"), "rb").read()
    off = struct.unpack_from("<I", src, 10)[0]
    bpp = struct.unpack_from("<H", src, 28)[0]
    hh = struct.unpack_from("<i", src, 22)[0]
    stride = ((w * bpp + 31) // 32) * 4
    out = bytearray(src[:off])
    for yy in range(hh - 1, -1, -1):
        out += src[off + yy * stride:off + (yy + 1) * stride]
    struct.pack_into("<i", out, 22, -hh)
    open(os.path.join(OUT, "topdown.bmp"), "wb").write(out)
    files["topdown.bmp"] = (rgb, False)
    save(bw, "bw.bmp")
    save(rgb.convert("P", palette=Image.ADAPTIVE, colors=256), "pal.bmp")
    rgb32 = rgba.copy()
    rgb32.save(os.path.join(OUT, "rgba.bmp"))
    files["rgba.bmp"] = (rgb32, False)   # BI_RGB alpha is ignored
    save(rgb, "rgb.tga")
    rgb.save(os.path.join(OUT, "rle.tga"), compression="tga_rle")
    files["rle.tga"] = (rgb, False)
    rgba.save(os.path.join(OUT, "rgba.tga"))
    files["rgba.tga"] = (rgba, True)
    save(rgb, "rgb.ppm")
    save(gray, "gray.pgm")
    save(bw, "bw.pbm")
    with open(os.path.join(OUT, "ascii.ppm"), "w") as f:
        f.write("P3\n%d %d\n255\n" % (w, h))
        for y in range(h):
            for x in range(w):
                f.write("%d %d %d " % p[x, y])
    files["ascii.ppm"] = (rgb, False)

    gif_src = rgb.convert("P", palette=Image.ADAPTIVE, colors=256)
    save(gif_src, "plain.gif")
    save(gif_src, "inter.gif", interlace=True)
    gif_src.save(os.path.join(OUT, "trans.gif"), transparency=5)
    files["trans.gif"] = (Image.open(os.path.join(OUT, "trans.gif")).convert("RGBA"), True)
    # Our decoder shows the first frame; PIL's first frame matches this source.
    frames = [pal.copy()]
    p2 = pal.copy()
    for y in range(h):
        for x in range(w):
            p2.putpixel((x, y), (x + y + 3) % 16)
    frames.append(p2)
    pal.save(os.path.join(OUT, "anim.gif"), save_all=True,
             append_images=frames[1:], duration=100, loop=0, transparency=5)
    files["anim.gif"] = (Image.open(os.path.join(OUT, "anim.gif")).convert("RGBA"), True)

    jpegs = []
    for name, kw in [("j444.jpg", dict(quality=90, subsampling=0)),
                     ("j422.jpg", dict(quality=90, subsampling=1)),
                     ("j420.jpg", dict(quality=90, subsampling=2)),
                     ("jgray.jpg", None)]:
        src = gray if kw is None else rgb
        src.save(os.path.join(OUT, name), **(kw or {"quality": 90}))
        ref = src.convert("RGB")
        ref.save(os.path.join(OUT, "ref", name + ".ppm"))
        jpegs.append(name)
    rgb.save(os.path.join(OUT, "jrst.jpg"), quality=90, restart_marker_blocks=2)
    rgb.save(os.path.join(OUT, "ref", "jrst.jpg.ppm"))
    jpegs.append("jrst.jpg")
    rgb.save(os.path.join(OUT, "prog.jpg"), quality=90, progressive=True)

    # inflate vectors
    raw = (b"the quick brown fox jumps over the lazy dog\n" * 500) + bytes(range(256)) * 80
    open(os.path.join(OUT, "inflate.raw"), "wb").write(raw)
    open(os.path.join(OUT, "inflate.z"), "wb").write(zlib.compress(raw, 9))
    open(os.path.join(OUT, "inflate.gz"), "wb").write(gzip.compress(raw, 9))

    exp = []
    for name, (im, alpha) in files.items():
        exp.append((name, im.width, im.height, fnv(im, alpha), 1 if alpha else 0))
    with open(os.path.join(OUT, "expected.h"), "w") as f:
        f.write("/* Generated by tools/gen_img_tests.py. */\n")
        f.write("static const struct img_exp {\n"
                "\tconst char *path;\n"
                "\tunsigned int w, h;\n"
                "\tunsigned int hash;\n"
                "\tint alpha;\n"
                "} img_exp[] = {\n")
        for name, ww, hh, hhsh, aa in sorted(exp):
            f.write("\t{ \"%s\", %d, %d, 0x%08xu, %d },\n" % (name, ww, hh, hhsh, aa))
        f.write("};\n")
        f.write("#define IMG_EXP_COUNT (sizeof(img_exp) / sizeof(img_exp[0]))\n")
        f.write("static const char *const img_jpeg[] = {\n")
        for name in jpegs:
            f.write("\t\"%s\",\n" % name)
        f.write("};\n")
        f.write("#define IMG_JPEG_COUNT (sizeof(img_jpeg) / sizeof(img_jpeg[0]))\n")

if __name__ == "__main__":
    main()
