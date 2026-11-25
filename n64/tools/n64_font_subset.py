#!/usr/bin/env python3
"""Cut STK's CJK fallback font down to the symbols the game's own text uses
(run by n64/make_rom.sh).

STK reads each font file whole into RAM, so wqy-microhei.ttf (4.6 MB, the
fallback for Chinese and Japanese) cannot be loaded on the N64. But it is
also where the on-screen keyboard's Enter, Shift and Backspace keys
(U+21B2, U+21E7, U+21E6), the lobby's stars and the track screen's ticks
come from: Cantarell has none of them, and without a fallback they are drawn
as "?". The subset keeps those (a few KB); CJK text stays undisplayable.

    n64_font_subset.py <font in> <font out>
"""
import sys

from fontTools import subset
from fontTools.ttLib import TTFont

# found by scanning data/gui and the string literals of src/ for characters
# Cantarell lacks; arrows and a few marks added for good measure
SYMBOLS = '↲⇦⇧★☆✔✖✓←↑→↓•×…'


def main():
    src, dst = sys.argv[1:3]
    opts = subset.Options()
    opts.layout_features = ['*']
    opts.name_IDs = ['*']
    opts.notdef_outline = True
    font = TTFont(src, fontNumber=0)
    sub = subset.Subsetter(opts)
    sub.populate(text=SYMBOLS)
    sub.subset(font)
    font.save(dst)


if __name__ == '__main__':
    main()
