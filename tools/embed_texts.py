#!/usr/bin/env python3
"""Generate ``source/Corpus.cpp`` from ``texts/*.txt``.

The output is committed rather than generated at build time, so a build needs no
Python and CI needs no network. ``tools/verify.sh`` regenerates and diffs, which
is what stops the committed file drifting from its source.

**Byte arrays, not string literals.** MSVC rejects a string literal longer than
65535 bytes -- a hard error, and one that shows up only on the Windows build,
which is to say only after the macOS build has already gone green.

Usage::

    tools/embed_texts.py            # write source/Corpus.cpp
    tools/embed_texts.py --check    # exit 1 if the committed file is stale
"""

import argparse
import pathlib
import sys

REPO = pathlib.Path(__file__).resolve().parent.parent
TEXTS = REPO / "texts"
TARGET = REPO / "source" / "Corpus.cpp"

# Must match the order of downpour::Work in Corpus.h -- the values are
# serialised into compositions.
WORKS = [
    ("Alice", "alice", "Alice in Wonderland"),
    ("Republic", "republic", "Plato - The Cave"),
    ("Hamlet", "hamlet", "Hamlet III.i"),
    ("Discourse", "descartes", "Descartes - The Cogito"),
]

HEADER = '''#include "Corpus.h"

/**
    GENERATED FILE -- do not edit.

    Written by `tools/embed_texts.py` from `texts/*.txt`. Edit those and
    regenerate; `tools/verify.sh` fails if this file and they disagree.

    See Corpus.h for why these are byte arrays rather than string literals, and
    `texts/SOURCES.md` for what each passage is and where it came from.
*/
namespace downpour
{
namespace
{
'''

FOOTER = '''
} // namespace

const char* WorkText( Work work, size_t& length )
{
	switch( work )
	{
%(cases)s	default:
		length = 0;
		return nullptr;
	}
}

const char* WorkTitle( Work work )
{
	switch( work )
	{
%(titles)s	default:
		return "";
	}
}

} // namespace downpour
'''


def emit_array(name: str, data: bytes) -> str:
    lines = [f"const char k{name}[] = {{"]
    for start in range(0, len(data), 20):
        chunk = data[start:start + 20]
        lines.append("\t" + "".join(f"{b:4d}," for b in chunk))
    lines.append("};")
    lines.append(f"const size_t k{name}Length = sizeof( k{name} );")
    lines.append("")
    return "\n".join(lines)


def build() -> str:
    body = [HEADER]
    cases = []
    titles = []

    for enum, filename, title in WORKS:
        path = TEXTS / f"{filename}.txt"
        data = path.read_text(encoding="utf-8").strip().encode("ascii")
        body.append(emit_array(enum, data))
        cases.append(f"\tcase Work::{enum}:\n"
                     f"\t\tlength = k{enum}Length;\n"
                     f"\t\treturn k{enum};\n")
        titles.append(f'\tcase Work::{enum}:\n\t\treturn "{title}";\n')

    body.append(FOOTER % {"cases": "".join(cases), "titles": "".join(titles)})
    return "".join(body)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()

    generated = build()

    if args.check:
        current = TARGET.read_text(encoding="utf-8") if TARGET.exists() else ""
        if current != generated:
            print("source/Corpus.cpp is stale -- run tools/embed_texts.py",
                  file=sys.stderr)
            sys.exit(1)
        print(f"Corpus.cpp up to date ({len(generated):,} bytes)")
        sys.exit(0)

    TARGET.write_text(generated, encoding="utf-8")
    print(f"wrote {TARGET.relative_to(REPO)} ({len(generated):,} bytes)")
