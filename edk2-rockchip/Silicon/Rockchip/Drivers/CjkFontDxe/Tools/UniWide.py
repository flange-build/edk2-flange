#!/usr/bin/env python3
#
# Mark up and check the zh-Hans strings of UNI files.
#
# The display engine and GraphicsConsoleDxe take a character's width from the
# \wide and \narrow markers in the string, never from the glyph. Without them
# a line of hanzi is laid out one column per character and drawn two columns
# per character, and everything after it lands on top of it. Writing the
# markers by hand is error prone, so translations are written plain and this
# tool puts the markers in: \wide in front of every run of wide characters,
# \narrow after it. Each quoted literal is closed in narrow mode, because the
# console keeps the wide attribute across OutputString() calls.
#
# Every string of a file that has zh-Hans at all needs a zh-Hans entry, even
# one that stays in English. StrGather leaves a string that a language lacks
# out of that language's package, and HiiGetString() does not fall back to
# another language for a single string: in Chinese, the string simply is not
# there. So the tool also gives each such string a copy of its en-US text.
#
# With --check nothing is rewritten; instead the tool fails if a zh-Hans
# string is missing, is not marked up the way it would mark it, uses a
# character no font has a glyph for, disagrees with its en-US original about
# printf-style conversions, or lives in a file without '#langdef zh-Hans'.
# --missing lists the strings whose zh-Hans is a copy of the English.
#
# Usage:
#   UniWide.py [--check] [--missing] --edk2 <edk2 tree> file.uni ...
#
# Copyright (c) 2026, Eric Wu <eric3u@outlook.com>
#
# SPDX-License-Identifier: BSD-2-Clause-Patent
#

import argparse
import os
import re
import sys

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from CjkWidth import is_wide  # noqa: E402

LANG = 'zh-Hans'
WIDE = '\\wide'
NARROW = '\\narrow'

LITERAL = re.compile(r'"((?:\\.|[^"\\])*)"')
TOKEN = re.compile(r'"(?:\\.|[^"\\])*"|//[^\n]*|#\w+|[^\s"]+')
CONVERSION = re.compile(r'%[-+ 0#,]*(?:\*|\d+)?(?:\.(?:\*|\d+))?[lLhN]*[a-zA-Z%]')

HERE = os.path.dirname(os.path.abspath(__file__))


def font_code_points(edk2):
    cps = set(range(0x20, 0x7F))
    sources = [
        os.path.join(HERE, '..', 'CjkFontGlyphs.c'),
        os.path.join(edk2, 'MdeModulePkg/Universal/Console/GraphicsConsoleDxe/LaffStd.c'),
    ]
    text_out = open(os.path.join(edk2, 'MdePkg/Include/Protocol/SimpleTextOut.h')).read()
    macros = {m.group(1): int(m.group(2), 16) for m in
              re.finditer(r'#define\s+(\w+)\s+0x([0-9a-fA-F]+)', text_out)}
    for path in sources:
        for m in re.finditer(r'^\s*\{\s*(?:\(CHAR16\))?(0x[0-9a-fA-F]+|\w+)\s*,',
                             open(path).read(), re.M):
            token = m.group(1)
            cps.add(int(token, 16) if token.startswith('0x') else macros[token])
    return cps


def split_escapes(body):
    """Split a literal body into characters, keeping backslash escapes whole."""
    out = []
    i = 0
    while i < len(body):
        if body.startswith(WIDE, i):
            out.append(WIDE)
            i += len(WIDE)
        elif body.startswith(NARROW, i):
            out.append(NARROW)
            i += len(NARROW)
        elif body[i] == '\\' and i + 1 < len(body):
            out.append(body[i:i + 2])
            i += 2
        else:
            out.append(body[i])
            i += 1
    return out


def mark_up(body):
    wide = False
    out = []
    for piece in split_escapes(body):
        if piece in (WIDE, NARROW):
            continue
        piece_wide = len(piece) == 1 and is_wide(piece)
        if piece_wide != wide:
            out.append(WIDE if piece_wide else NARROW)
            wide = piece_wide
        out.append(piece)
    if wide:
        out.append(NARROW)
    return ''.join(out)


def plain(body):
    return ''.join(p for p in split_escapes(body) if p not in (WIDE, NARROW))


class UniFile:
    """Every quoted literal of a UNI file, attributed to its string and language."""

    def __init__(self, path):
        self.path = path
        raw = open(path, 'rb').read()
        if raw[:2] in (b'\xff\xfe', b'\xfe\xff'):
            self.encoding = 'utf-16'
        elif raw[:3] == b'\xef\xbb\xbf':
            self.encoding = 'utf-8-sig'
        else:
            self.encoding = 'utf-8'
        self.text = raw.decode(self.encoding)
        self.newline = '\r\n' if '\r\n' in self.text else '\n'
        self.literals = []      # (start, end, name, lang)
        self.blocks = []        # one per #string: name, column, literals by language
        self.langdefs = set()
        self._scan()

    def _scan(self):
        name = None
        lang = None
        pending = None
        block = None
        for m in TOKEN.finditer(self.text):
            tok = m.group(0)
            if tok.startswith('//'):
                continue
            if pending is not None:
                if pending == '#string':
                    name, lang = tok, None
                    block = {'name': name, 'column': None, 'langs': {}}
                    self.blocks.append(block)
                elif pending == '#language':
                    lang = tok
                    if block is not None and block['column'] is None:
                        line_start = self.text.rfind('\n', 0, m.start()) + 1
                        block['column'] = self.text.rfind('#language', line_start, m.start()) - line_start
                elif pending == '#langdef':
                    self.langdefs.add(tok)
                    name, lang, block = '#langdef', tok, None
                pending = None
                continue
            if tok in ('#string', '#language', '#langdef'):
                pending = tok
            elif tok.startswith('"'):
                if name is not None and lang is not None:
                    self.literals.append((m.start(), m.end(), name, lang))
                    if block is not None:
                        block['langs'].setdefault(lang, []).append((m.start(), m.end()))
            elif tok.startswith('#'):
                name, lang, block = None, None, None

    def untranslated(self):
        """#string blocks that have en-US text but no zh-Hans."""
        return [b for b in self.blocks if 'en-US' in b['langs'] and LANG not in b['langs']]

    def strings(self, lang):
        """name -> concatenated literal bodies, for one language."""
        result = {}
        for start, end, name, literal_lang in self.literals:
            if literal_lang == lang and name != '#langdef':
                result[name] = result.get(name, '') + self.text[start + 1:end - 1]
        return result

    def rewrite(self):
        #
        # Edits as (offset, replaced length, new text), applied back to front:
        # zh-Hans literals marked up in place, and a copy of the English put
        # at the end of the last line of every string that has no zh-Hans.
        #
        edits = []
        for start, end, name, lang in self.literals:
            if lang == LANG:
                body = self.text[start + 1:end - 1]
                edits.append((start + 1, len(body), mark_up(body)))
        for block in self.untranslated():
            last_end = max(end for literals in block['langs'].values() for _, end in literals)
            line_end = self.text.find(self.newline, last_end)
            if line_end < 0:
                line_end = len(self.text)
            head = ' ' * block['column'] + f'#language {LANG}  '
            lines = []
            for i, (start, end) in enumerate(block['langs']['en-US']):
                lines.append((head if i == 0 else ' ' * len(head)) + self.text[start:end])
            edits.append((line_end, 0, ''.join(self.newline + line for line in lines)))
        text = self.text
        for offset, length, new in sorted(edits, reverse=True):
            text = text[:offset] + new + text[offset + length:]
        return text

    def save(self, text):
        with open(self.path, 'wb') as f:
            f.write(text.encode(self.encoding))


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--check', action='store_true', help='report problems instead of rewriting')
    ap.add_argument('--missing', action='store_true', help='also list en-US strings with no zh-Hans')
    ap.add_argument('--edk2', required=True, help='edk2 tree, for LaffStd.c and SimpleTextOut.h')
    ap.add_argument('files', nargs='+')
    args = ap.parse_args()

    covered = font_code_points(args.edk2)
    errors = 0
    total = translated = 0

    def report(path, msg):
        nonlocal errors
        errors += 1
        print(f'{path}: {msg}')

    for path in args.files:
        uni = UniFile(path)
        zh = uni.strings(LANG)
        en = uni.strings('en-US')
        total += len(en)
        translated += sum(1 for n in en if n in zh and plain(zh[n]) != plain(en[n]))

        if not zh and LANG not in uni.langdefs:
            if args.missing:
                print(f'{path}: no {LANG} strings')
            continue
        if LANG not in uni.langdefs:
            report(path, f"has {LANG} strings but no '#langdef {LANG}'")

        for block in uni.untranslated() if args.check else []:
            report(path, f"{block['name']}: no {LANG} entry, so the string is missing in "
                         f"Chinese (run without --check to copy the English)")

        if not args.check:
            text = uni.rewrite()
            if text != uni.text:
                uni.save(text)
                print(f'{path}: marked up')
            continue

        for start, end, name, lang in uni.literals:
            if lang != LANG:
                continue
            body = uni.text[start + 1:end - 1]
            if mark_up(body) != body:
                report(path, f'{name}: \\wide/\\narrow markup is off (run without --check)')
            for piece in split_escapes(body):
                if len(piece) == 1 and ord(piece) not in covered:
                    report(path, f'{name}: no glyph for U+{ord(piece):04X} {piece}')

        for name, body in zh.items():
            if name in en:
                want = CONVERSION.findall(plain(en[name]))
                got = CONVERSION.findall(plain(body))
                if want != got:
                    report(path, f'{name}: conversions {got} differ from en-US {want}')
            else:
                report(path, f'{name}: {LANG} string has no en-US original')

        if args.missing:
            for name in en:
                if name in zh and plain(zh[name]) == plain(en[name]):
                    print(f'{path}: {name}: kept in English: {plain(en[name])!r}')

    print(f'{translated}/{total} strings translated, {errors} problem(s)')
    return 1 if errors else 0


if __name__ == '__main__':
    sys.exit(main())
