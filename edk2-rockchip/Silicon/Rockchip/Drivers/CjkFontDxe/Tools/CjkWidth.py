#
# Which UCS-2 characters take two console columns.
#
# The HII display engine never measures glyphs; it trusts the \wide and
# \narrow markers in a string. A terminal emulator does the opposite and
# decides by itself, going by Unicode's East Asian Width. Both have to agree
# or the menu shears, so this table is the one both sides are held to: the
# BMP ranges whose East Asian Width is W or F, the same set wcwidth() treats
# as double width. TerminalDxe carries a C copy of it.
#
# Copyright (c) 2026, Eric Wu <eric3u@outlook.com>
#
# SPDX-License-Identifier: BSD-2-Clause-Patent
#

WIDE_RANGES = (
    (0x1100, 0x115F),   # Hangul Jamo initial consonants
    (0x2E80, 0x303E),   # CJK radicals, Kangxi, ideographic description, CJK symbols and punctuation
    (0x3041, 0x33FF),   # Hiragana, Katakana, Bopomofo, Hangul compatibility Jamo, Kanbun, CJK strokes, enclosed and compatibility
    (0x3400, 0x4DBF),   # CJK Unified Ideographs Extension A
    (0x4E00, 0x9FFF),   # CJK Unified Ideographs
    (0xA000, 0xA4CF),   # Yi syllables and radicals
    (0xA960, 0xA97F),   # Hangul Jamo Extended-A
    (0xAC00, 0xD7A3),   # Hangul syllables
    (0xF900, 0xFAFF),   # CJK Compatibility Ideographs
    (0xFE10, 0xFE19),   # Vertical forms
    (0xFE30, 0xFE6F),   # CJK compatibility forms, small form variants
    (0xFF00, 0xFF60),   # Fullwidth ASCII variants
    (0xFFE0, 0xFFE6),   # Fullwidth symbol variants
)


def is_wide(ch):
    cp = ord(ch)
    return any(lo <= cp <= hi for lo, hi in WIDE_RANGES)
