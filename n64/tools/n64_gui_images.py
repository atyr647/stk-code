#!/usr/bin/env python3
"""Prepare a client ROM's images for the N64 (run by n64/make_rom.sh).

The N64's GL draws textures of at most 64x64 (TMEM is 4 KB), and STK's
texture code resizes every image it loads to power-of-two sides of at most
the driver's maximum (GE::getResizingTarget). It does so after decoding the
file at full size, which for STK's desktop-sized GUI art (a 2048x1024 skin
background) needs megabytes the console does not have.

So each image is stored at exactly the size STK would resize it to: each
side rounded up to a power of two, then capped at 64. Nothing is resized at
run time, and decoding takes a few KB.

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


def target(size, cap):
    return tuple(min(pot_up(v), cap) for v in size)


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

    resized = 0
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
            t = target(im.size, a.max)
            if t == im.size:
                continue
            fmt = im.format
            if im.mode not in ('RGB', 'RGBA', 'L', 'LA'):
                im = im.convert('RGBA' if 'transparency' in im.info or
                                im.mode in ('P', 'PA') else 'RGB')
            im = im.resize(t, Image.LANCZOS)
            if fmt == 'JPEG':
                im.convert('RGB').save(p, 'JPEG', quality=92)
            else:
                im.save(p, 'PNG', optimize=True)
            resized += 1
    print('n64_gui_images: %d images resized, %d skin elements rescaled'
          % (resized, skins))


if __name__ == '__main__':
    main()
