#!/usr/bin/env python3
"""Prepare a client ROM's images for the N64 (run by n64/make_rom.sh).

The N64's GL draws textures of at most 64x64 (TMEM is 4 KB), and STK's
texture code resizes every image it loads to power-of-two sides of at most
the driver's maximum (GE::getResizingTarget). It does so after decoding the
file at full size, which for STK's desktop-sized GUI art (a 2048x1024 skin
background) needs megabytes the console does not have.

So each image is stored at the size the console will draw it at, and with
the colours it can keep (n64/compat/src/sdl_n64.c picks the texture format
from the number of colours it finds, without quantising anything itself):

  * 3D textures (tracks, karts, models, ...): 16 colours, which the console
    keeps as CI4 at up to 64x64 in 2 KB -- the budget of
    n64/tools/stk_n64_assets.py. A 16-bit (RGBA16) texture would have to
    be halved to 64x32 to fit TMEM, and take twice the RAM.
  * GUI images (gui/, skins/): 256 colours, kept as CI8 at most 2048
    texels (64x32 for a square icon), the most a texture with more than 16
    colours can have. The console gets there by dropping every other column
    (or row); the file is filtered at the kept resolution with each texel
    repeated, so what it keeps is a properly filtered image, and the file
    keeps the image's proportions, which STK lays icons out from.

Single-channel images stay single-channel (STK draws them as white, with
the image as alpha). Grey images with soft alpha (shadows, smoke) keep it: the console stores
them as IA8 (4 bits of grey, 4 of alpha). Otherwise colours are reduced
with dithering; alpha becomes one bit (the console's
texture formats have one bit of alpha), and transparent texels all take the
average colour of the opaque ones, so they cost one palette entry and
filtering does not bleed a stray colour into edges. Images are written as
PNG whatever their name (STK recognises PNG data in a .jpg by its content):
JPEG compression would bring back the colours just removed.

The skins give 9-slice borders in pixels of their images (left_border, ...,
see data/skins/SKIN_MAKING.md); STK applies them to the texture's size. They
are scaled by the same per-axis factor as the image, so every element keeps
its proportions.

    n64_gui_images.py <rom fs data dir> [--max 64]
"""
import argparse
import os
import re
import sys

from PIL import Image

EXTS = ('.png', '.jpg', '.jpeg')


def pot_up(n):
    p = 1
    while p < n:
        p *= 2
    return p


GUI_TEXELS = 2048          # sdl_n64.c: CI8 / RGBA16 in TMEM
GUI_COLOURS = 256
TEXTURE_COLOURS = 16       # CI4


def is_gui(path):
    parts = path.replace(os.sep, '/').split('/')
    return 'gui' in parts or 'skins' in parts


def target(size, cap):
    return tuple(min(pot_up(v), cap) for v in size)


def kept(size):
    """What the console keeps of a GUI image of `size` with more than 16
    colours: it halves the longer side (the width when square) until 2048
    texels fit TMEM, by dropping every other column or row (sdl_n64.c,
    halve())."""
    w, h = size
    while w * h > GUI_TEXELS:
        if w >= h:
            w //= 2
        else:
            h //= 2
    return (w, h)


def for_console_halving(im, size):
    """im resized to `size` for a console that keeps only kept(size) of it:
    filtered at that resolution, then each texel repeated, so the texels the
    console keeps are exactly the filtered ones -- and the file keeps its
    proportions, from which STK sizes icons."""
    k = kept(size)
    if k == size:
        return im.resize(size, Image.LANCZOS)
    return im.resize(k, Image.LANCZOS).resize(size, Image.NEAREST)


def soft_grey(im):
    """A grey image with soft alpha (a shadow, smoke, a glow mask): the
    console keeps it as IA8, 4 bits of grey and 4 of alpha, and its alpha
    must not be cut to one bit."""
    r, g, b, a = im.split()
    if not any(a.histogram()[16:240]):
        return False
    from PIL import ImageChops
    return (ImageChops.difference(r, g).getextrema()[1] <= 8 and
            ImageChops.difference(g, b).getextrema()[1] <= 8)


def reduce_colours(im, colours):
    """im (any mode) as RGBA with at most `colours` colours, 1-bit alpha;
    soft-alpha grey images are left as they are (soft_grey)."""
    im = im.convert('RGBA')
    if soft_grey(im):
        return im
    alpha = im.getchannel('A').point(lambda a: 255 if a >= 128 else 0)
    opaque = alpha.getbbox() is not None
    rgb = im.convert('RGB')
    transparent = alpha.getextrema()[0] == 0
    if transparent and opaque:
        # transparent texels: the opaque texels' average colour
        from PIL import ImageStat
        mean = tuple(int(round(v)) for v in ImageStat.Stat(rgb, alpha).mean)
        rgb.paste(Image.new('RGB', rgb.size, mean),
                  mask=alpha.point(lambda a: 255 - a))
    n = colours - 1 if transparent else colours
    # in the console's 5 bits per channel, so palette entries stay distinct
    rgb = rgb.point(lambda v: (v & 0xF8) | (v >> 5))
    q = rgb.quantize(colors=max(n, 1), method=Image.Quantize.MEDIANCUT,
                     dither=Image.Dither.FLOYDSTEINBERG).convert('RGB')
    if transparent and opaque:
        q.paste(Image.new('RGB', q.size, mean),
                mask=alpha.point(lambda a: 255 - a))
    out = q.convert('RGBA')
    out.putalpha(alpha)
    return out


def resolve(skin_dir, data_dir, name):
    for d in (skin_dir, os.path.join(data_dir, 'skins', 'common')):
        p = os.path.join(d, name)
        if os.path.isfile(p):
            return p
    return None


def rescale_skin(xml_path, data_dir, cap, sizes):
    """Scale the pixel borders of every element of a skin file."""
    skin_dir = os.path.dirname(xml_path)
    text = open(xml_path, encoding='utf-8').read()
    changed = 0

    def element(m):
        nonlocal changed
        tag = m.group(0)
        im = re.search(r'\bimage="([^"]+)"', tag)
        if not im or 'border=' not in tag:
            return tag
        path = resolve(skin_dir, data_dir, im.group(1))
        if not path:
            return tag
        w, h = sizes.setdefault(path, Image.open(path).size)
        tw, th = target((w, h), cap)
        sx, sy = tw / w, th / h

        def scale(attr, f):
            def sub(a):
                v = int(a.group(2))
                return '%s="%d"' % (a.group(1), max(0, round(v * f)))
            return re.sub(r'\b(%s)="(\d+)"' % attr, sub, tag_in_progress[0])

        tag_in_progress = [tag]
        for attr, f in (('left_border', sx), ('right_border', sx),
                        ('top_border', sy), ('bottom_border', sy)):
            tag_in_progress[0] = scale(attr, f)
        if tag_in_progress[0] != tag:
            changed += 1
        return tag_in_progress[0]

    out = re.sub(r'<[^<>]*>', element, text)
    if out != text:
        open(xml_path, 'w', encoding='utf-8').write(out)
    return changed


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('data')
    ap.add_argument('--max', type=int, default=64)
    a = ap.parse_args()

    # original sizes first: the skins' borders refer to them
    sizes = {}
    skins = 0
    for root, _, files in os.walk(os.path.join(a.data, 'skins')):
        for f in files:
            if f.endswith('.xml'):
                skins += rescale_skin(os.path.join(root, f), a.data, a.max,
                                      sizes)

    resized = reduced = 0
    for root, _, files in os.walk(a.data):
        for f in files:
            if not f.lower().endswith(EXTS):
                continue
            p = os.path.join(root, f)
            if os.path.getsize(p) == 0:
                continue                 # stand-in
            try:
                im = Image.open(p)
                im.load()
            except Exception as e:      # not an image STK could load either
                print('n64_gui_images: %s: %s' % (p, e), file=sys.stderr)
                continue
            gui = is_gui(os.path.relpath(p, a.data))
            t = target(im.size, a.max)
            if im.mode == 'L':
                # single channel: STK draws it as white with the image as
                # alpha (the classic skin's background over the blue clear
                # colour); the console keeps it as IA8 whole. Kept as is.
                if t != im.size:
                    im = im.resize(t, Image.LANCZOS)
                    resized += 1
                im.save(p, 'PNG', optimize=True)
                continue
            if im.mode not in ('RGB', 'RGBA', 'L', 'LA'):
                im = im.convert('RGBA' if 'transparency' in im.info or
                                im.mode in ('P', 'PA') else 'RGB')
            if gui and kept(t) != t:
                im = for_console_halving(im, t)
                resized += 1
            elif t != im.size:
                im = im.resize(t, Image.LANCZOS)
                resized += 1
            im = reduce_colours(im, GUI_COLOURS if gui else TEXTURE_COLOURS)
            im.save(p, 'PNG', optimize=True)
            reduced += 1
    print('n64_gui_images: %d images resized, %d colour-reduced, %d skin '
          'elements rescaled' % (resized, reduced, skins))


if __name__ == '__main__':
    main()
