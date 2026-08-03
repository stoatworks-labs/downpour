#!/usr/bin/env python3
"""Cut the four built-in passages out of their Project Gutenberg source files.

Run once, by hand, when a passage needs changing. The output -- ``texts/*.txt``
-- is committed, so a normal build (and CI) never touches the network.

Two things this does that matter legally as well as typographically:

* **It slices the body only.** Every passage is a line range strictly inside the
  work, so the Project Gutenberg header, footer and licence never enter the
  output. All four works are public domain in the US; with the PG boilerplate
  and trademark stripped, the text carries no further restriction. ``--check``
  asserts that no reference survived.

* **It flattens the typography** to what a glyph atlas can actually draw. Curly
  quotes, dashes and ligatures become their ASCII equivalents; italic
  underscores are dropped; whitespace runs collapse to one space. The rain
  renders a flat sequence of characters and has no lines, so line structure is
  not information here -- it is just a run of spaces that would show up as a
  hole in a column.

Usage::

    tools/extract_texts.py --source /path/to/gutenberg/downloads
    tools/extract_texts.py --check
"""

import argparse
import pathlib
import re
import sys
import unicodedata

REPO = pathlib.Path(__file__).resolve().parent.parent
OUT = REPO / "texts"

# (output name, gutenberg id, first line, last line, title, credit)
PASSAGES = [
    (
        "alice", 11, 58, 276,
        "Alice's Adventures in Wonderland",
        "Lewis Carroll, 1865. Chapter I, 'Down the Rabbit-Hole'.",
    ),
    (
        "republic", 1497, 18770, 19100,
        "The Republic, Book VII",
        "Plato, c. 375 BC. Translated by Benjamin Jowett, 1871. "
        "The allegory of the cave.",
    ),
    (
        "descartes", 59, 850, 1089,
        "Discourse on the Method, Part IV",
        "Rene Descartes, 1637. Translated by John Veitch, 1850. "
        "Methodical doubt, the dream argument and the cogito.",
    ),
    (
        "hamlet", 1524, 2767, 3066,
        "Hamlet, Act III Scene I",
        "William Shakespeare, c. 1600. 'To be, or not to be'.",
    ),
]

# Applied in order. The point is a stream a glyph atlas can draw, not a faithful
# transcription -- so anything that only carries typographic nuance is folded
# down to the nearest thing with a glyph.
REPLACEMENTS = [
    ("‘", "'"), ("’", "'"),          # single curly quotes
    ("“", '"'), ("”", '"'),          # double curly quotes
    ("—", " -- "), ("–", "-"),       # em, en dash
    ("…", "..."),                         # ellipsis
    ("æ", "ae"), ("Æ", "AE"),
    ("œ", "oe"), ("Œ", "OE"),
    (" ", " "),                           # no-break space
]

BANNED = re.compile(r"gutenberg|www\.|https?://", re.IGNORECASE)


def flatten(text: str) -> str:
    for src, dst in REPLACEMENTS:
        text = text.replace(src, dst)

    # Underscores are Gutenberg's italic marker, not punctuation in these works.
    text = text.replace("_", "")

    # Strip accents rather than dropping the letter: "Rene" beats "Ren" and both
    # beat a blank cell. Anything still non-ASCII after this has no business in
    # an English passage and goes.
    text = unicodedata.normalize("NFKD", text)
    text = "".join(c for c in text if ord(c) < 128)

    # The rain has no lines. Collapsing here rather than at load time means the
    # committed file is exactly what gets embedded.
    return re.sub(r"\s+", " ", text).strip()


def extract(source: pathlib.Path) -> int:
    OUT.mkdir(exist_ok=True)
    manifest = ["# Sources for the built-in passages",
                "",
                "All four works are in the public domain in the United States.",
                "Each file here is a body-only excerpt: the Project Gutenberg",
                "header, footer and licence are never included, and `--check`",
                "asserts that no reference to them survives. Regenerate with",
                "`tools/extract_texts.py`.",
                ""]

    for name, book, first, last, title, credit in PASSAGES:
        src = source / f"{book}.txt"
        if not src.exists():
            print(f"missing {src}", file=sys.stderr)
            return 1

        lines = src.read_text(encoding="utf-8").splitlines()
        body = flatten("\n".join(lines[first - 1:last]))

        if BANNED.search(body):
            print(f"{name}: boilerplate survived the slice", file=sys.stderr)
            return 1

        (OUT / f"{name}.txt").write_text(body + "\n", encoding="utf-8")
        print(f"{name:12s} {len(body):>7,} chars  {title}")
        manifest.append(f"- **{name}.txt** -- {title}. {credit} "
                        f"Project Gutenberg ebook #{book}, lines {first}-{last}.")

    (OUT / "SOURCES.md").write_text("\n".join(manifest) + "\n", encoding="utf-8")
    return 0


def check() -> int:
    bad = 0
    for name, *_ in PASSAGES:
        path = OUT / f"{name}.txt"
        if not path.exists():
            print(f"missing {path}", file=sys.stderr)
            bad += 1
            continue
        text = path.read_text(encoding="utf-8")
        if BANNED.search(text):
            print(f"{name}: contains boilerplate", file=sys.stderr)
            bad += 1
        if any(ord(c) > 126 for c in text):
            print(f"{name}: contains non-ASCII", file=sys.stderr)
            bad += 1
    if bad == 0:
        print(f"{len(PASSAGES)} passages clean")
    return 1 if bad else 0


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=pathlib.Path)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()

    if args.check:
        sys.exit(check())
    if not args.source:
        parser.error("--source is required unless --check")
    sys.exit(extract(args.source))
