#!/usr/bin/env python3
"""Turn source/GlyphData.cpp and texts/*.txt into demo/data.js.

Two things the demo needs and must not have its own copy of:

**The drawn face.** 12x12 ASCII art, top row first, in GlyphData.cpp. It is
extracted rather than redrawn because a katakana with a stroke in the wrong
place still compiles, still renders, and still looks vaguely Japanese — the only
way that is caught is by looking at the atlas, and there is no reason for this
page to have a second atlas that could be wrong on its own.

**The four public domain passages.** Read straight from texts/, which is what
tools/embed_texts.py generates Corpus.cpp from, so the page and the plugin are
reading the same files rather than two transcriptions of them.

    python3 demo/tools/extract_data.py
    python3 demo/tools/extract_data.py --check
"""
from __future__ import annotations

import argparse
import json
import pathlib
import re
import sys

REPO = pathlib.Path(__file__).resolve().parents[2]
GLYPH_SOURCE = REPO / "source" / "GlyphData.cpp"
TEXTS = REPO / "texts"
TARGET = REPO / "demo" / "data.js"

BITMAP_SIZE = 12

# The glyph label comments carry the character itself, so a comment can be `//"`
# — an unbalanced quote that pairs with the next line's and scrambles every
# glyph after it. Strip comments first. The art is only '.', '#' and spaces, so
# there is no '//' inside a string literal for this to damage.
LINE_COMMENT = re.compile(r"//[^\n]*")

# A glyph names its codepoint either as hex (the katakana) or as a character
# literal (the latin, digits and punctuation), including the two escaped forms
# `'\''` and `'\\'`. Matching only the hex spelling silently yields 49 of the
# 140 glyphs and an atlas that is missing its entire ASCII half — which looks
# like a broken character set rather than a broken parser.
KEY = r"(?:0x[0-9A-Fa-f]+|'(?:\\.|[^'\\])')"
ENTRY = re.compile(
    r"\{\s*(%s)\s*,\s*\{\s*((?:\"[^\"]*\"\s*,?\s*){%d})\}\s*,?\s*\}" % (KEY, BITMAP_SIZE)
)
ROW = re.compile(r'"([^"]*)"')

ESCAPES = {"\\\\": ord("\\"), "\\'": ord("'"), "\\n": ord("\n"), "\\t": ord("\t"), "\\0": 0}


def codepoint_of(key: str) -> int:
    if key.startswith("0x"):
        return int(key, 16)
    inner = key[1:-1]
    if inner.startswith("\\"):
        if inner not in ESCAPES:
            raise SystemExit(f"unhandled character escape in GlyphData.cpp: {key}")
        return ESCAPES[inner]
    return ord(inner)

# The four works, in the order of the Work enum in source/Corpus.h. That order
# is serialised into compositions, so entries may be appended but never
# reordered.
WORKS = [
    ("alice.txt", "Alice in Wonderland"),
    ("republic.txt", "Plato - The Cave"),
    ("hamlet.txt", "Hamlet III.i"),
    ("descartes.txt", "Descartes - The Cogito"),
]

# The plugin caps a document at kMaxDocument codepoints. Excerpts, not complete
# works: at any legible rain speed nobody was ever going to reach the end of the
# Republic, and what an excerpt buys is that the passage on screen is the passage
# worth having on screen.
MAX_DOCUMENT = 200000


def parse_glyphs() -> list[tuple[int, list[int]]]:
    text = GLYPH_SOURCE.read_text(encoding="utf-8")
    start = text.index("const BitmapArt kFace[]")
    end = text.index("} // namespace", start)
    body = LINE_COMMENT.sub("", text[start:end])

    glyphs: list[tuple[int, list[int]]] = []
    for match in ENTRY.finditer(body):
        codepoint = codepoint_of(match.group(1))
        rows = ROW.findall(match.group(2))
        if len(rows) != BITMAP_SIZE:
            raise SystemExit(f"glyph {codepoint:#06x} has {len(rows)} rows, expected {BITMAP_SIZE}")

        # ParseArt flips here and nowhere else: row 0 of the art is the TOP,
        # row 0 of a Bitmap is the BOTTOM, to match GL's texture origin. If this
        # happens zero times or twice the picture is upside down in a way that
        # is surprisingly easy to look straight past.
        bits = [0] * BITMAP_SIZE
        for art_row, line in enumerate(rows):
            row = BITMAP_SIZE - 1 - art_row
            value = 0
            for col, ch in enumerate(line[:BITMAP_SIZE]):
                if ch == "#":
                    value |= 1 << col
            bits[row] = value
        glyphs.append((codepoint, bits))

    return glyphs


def render(glyphs: list[tuple[int, list[int]]], works: list[tuple[str, str]]) -> str:
    lines = [
        "/**",
        " * The built-in face and the four public domain passages, extracted from",
        " * source/GlyphData.cpp and texts/.",
        " *",
        " * GENERATED — do not edit. Run `python3 demo/tools/extract_data.py`, and",
        " * `--check` to prove this copy is current.",
        " */",
        "",
        f"export const BITMAP_SIZE = {BITMAP_SIZE};",
        "",
        "/// [codepoint, ...twelve rows of bits]. **Row 0 is the bottom**, bit c is",
        "/// column c from the left — what ParseArt leaves behind. Slot 0 is blank and",
        "/// must stay slot 0: an unknown codepoint, an inactive cell and the gap",
        "/// between two drops all resolve there, which is what lets the shader draw",
        "/// nothing without a branch.",
        "export const FACE = [",
    ]
    for codepoint, bits in glyphs:
        lines.append(f"  [0x{codepoint:04X}, {', '.join(str(b) for b in bits)}],")
    lines.append("];")
    lines.append("")
    lines.append("/// Title and text, in the order of the Work enum in source/Corpus.h.")
    lines.append("export const WORKS = [")
    for filename, title in works:
        text = (TEXTS / filename).read_text(encoding="utf-8")
        if len(text) > MAX_DOCUMENT:
            text = text[:MAX_DOCUMENT]
        lines.append(f"  {{ title: {json.dumps(title)}, text: {json.dumps(text)} }},")
    lines.append("];")
    lines.append("")
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true", help="exit non-zero if data.js is stale")
    args = parser.parse_args()

    glyphs = parse_glyphs()
    if not glyphs:
        raise SystemExit("no glyphs parsed — has GlyphData.cpp's layout changed?")
    if glyphs[0][0] != 0x0000:
        raise SystemExit("slot 0 is not the blank glyph — the shader depends on that")

    missing = [name for name, _ in WORKS if not (TEXTS / name).exists()]
    if missing:
        raise SystemExit(f"missing text files: {', '.join(missing)}")

    text = render(glyphs, WORKS)

    if args.check:
        if not TARGET.exists() or TARGET.read_text(encoding="utf-8") != text:
            print(f"demo/data.js is out of date — run {pathlib.Path(__file__).name}", file=sys.stderr)
            return 1
        print(f"demo/data.js matches ({len(glyphs)} glyphs, {len(WORKS)} works)")
        return 0

    TARGET.write_text(text, encoding="utf-8")
    print(f"wrote {TARGET.relative_to(REPO)} — {len(glyphs)} glyphs, {len(WORKS)} works")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
