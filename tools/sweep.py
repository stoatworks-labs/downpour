#!/usr/bin/env python3
"""Render every parameter at both ends of its range and fail if any made no
difference.

**This is the only thing in the repo that catches a dead control**, and it is
not a theoretical risk. A GLSL uniform whose name does not match the C++ is
silently ignored -- `glGetUniformLocation` returns -1 and `glUniform` on -1 is a
documented no-op -- so a slider can be stone dead while everything compiles,
links, loads and renders.

`dptest --rain` will not catch it. That mode checks the shader's arithmetic
against `Rain.cpp`, and a uniform that is never set makes both sides agree
perfectly on the wrong thing. `--readback` will not catch it either, because it
only ever exercises one setting.

Downpour shipped exactly this bug during development: `StreamSize` was declared
`ivec2` in GLSL and set through `FFGLShader::Set( const char*, float, float )`,
which is a `glUniform2f` against an integer uniform -- a GL_INVALID_OPERATION
that leaves the value at zero, with nothing anywhere for the plugin to notice.
Every glyph resolved to the blank slot.

Usage::

    tools/sweep.py [--build BUILD_DIR] [--verbose]
"""

import argparse
import pathlib
import re
import subprocess
import sys
import tempfile
import zlib

REPO = pathlib.Path(__file__).resolve().parent.parent

# Parameters that only do anything on the effect bundle, so they are swept with
# `--effect` (which renders the same rain over a synthetic clip) rather than
# against the source, where they are correctly inert.
EFFECT_ONLY = {"Mix"}

# A system font that is present on every macOS install and is not the built-in
# face, so that sweeping Font File is a real before/after rather than two
# renders of the same fallback.
SYSTEM_FONTS = [
    "/System/Library/Fonts/Supplemental/Courier New.ttf",
    "/System/Library/Fonts/Menlo.ttc",
    "/System/Library/Fonts/Supplemental/Times New Roman.ttf",
]

# A pair of settings applied to every render so the sweep is looking at a frame
# where the parameter under test can actually do something. Without this, a
# sweep of Head Boost against a frame whose drops are all mid-trail measures
# nothing.
BASE = [
    "Density=1.0",
    "Trail=0.7",
    "Columns=0.45",
]


def read_png(path):
    """Decode a PNG to raw bytes. Enough of the format for our own writer's
    output -- 8-bit RGBA, one IDAT, filter 0 on every row."""
    data = path.read_bytes()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("not a PNG")

    pos = 8
    width = height = 0
    idat = b""
    while pos < len(data):
        length = int.from_bytes(data[pos:pos + 4], "big")
        kind = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + length]
        if kind == b"IHDR":
            width = int.from_bytes(body[0:4], "big")
            height = int.from_bytes(body[4:8], "big")
        elif kind == b"IDAT":
            idat += body
        pos += 12 + length

    raw = zlib.decompress(idat)
    stride = width * 4
    out = bytearray()
    for y in range(height):
        start = y * (stride + 1)
        if raw[start] != 0:
            raise ValueError("unexpected PNG filter")
        out += raw[start + 1:start + 1 + stride]
    return width, height, bytes(out)


def difference(a, b):
    """Mean absolute difference per channel, 0..255."""
    if len(a) != len(b):
        return 255.0
    total = sum(abs(x - y) for x, y in zip(a, b))
    return total / len(a)


def render(dptest, out, settings, extra, effect=False):
    args = [str(dptest), "--out", str(out), "--width", "480", "--height", "270",
            "--time", "6.25"]
    if effect:
        args.append("--effect")
    for setting in BASE + settings + extra:
        args += ["--set", setting]
    result = subprocess.run(args, capture_output=True, text=True)
    if result.returncode != 0:
        raise RuntimeError(f"dptest failed: {result.stderr.strip()}")
    return read_png(out)[2]


def parameters(dptest):
    result = subprocess.run([str(dptest), "--list"], capture_output=True, text=True)
    if result.returncode != 0:
        raise RuntimeError(f"dptest --list failed: {result.stderr.strip()}")

    found = []
    for line in result.stdout.splitlines()[1:]:
        # `--list` pads its columns, so two or more spaces is the separator.
        # Splitting on single whitespace would break every parameter whose name
        # contains a space, which is most of them.
        parts = re.split(r"\s{2,}", line.strip())
        if len(parts) < 3 or not parts[0].isdigit():
            continue
        found.append((parts[1].strip(), parts[2].strip()))
    return found


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", type=pathlib.Path, default=REPO / "build")
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args()

    dptest = args.build / "dptest"
    if not dptest.exists():
        print(f"{dptest} not found -- build with -DDOWNPOUR_BUILD_TOOLS=ON",
              file=sys.stderr)
        return 1

    found = parameters(dptest)
    if not found:
        print("dptest --list returned no parameters", file=sys.stderr)
        return 1

    dead = []
    checked = 0

    with tempfile.TemporaryDirectory() as scratch:
        scratch = pathlib.Path(scratch)
        low = scratch / "low.png"
        high = scratch / "high.png"

        # A real document for the Text File picker to load. Written here rather
        # than committed so the sweep has nothing to keep in sync.
        sample = scratch / "sample.txt"
        sample.write_text("MMMMMMMM " * 400, encoding="utf-8")

        font = next((f for f in SYSTEM_FONTS if pathlib.Path(f).exists()), None)

        for name, kind in found:
            effect = name in EFFECT_ONLY
            extra = []

            if kind in ("text", "file"):
                if name == "Custom Text":
                    # Only read when the Source dropdown is on Custom Text.
                    ends = [f"{name}=AAAAAAAA", f"{name}=::::::::"]
                    extra = ["Source=6"]
                elif name == "Text File":
                    # Source 7 is Text File. The two ends are "a file that
                    # loads" against "a path that does not exist", which also
                    # exercises the fallback rather than only the happy path.
                    ends = [f"{name}={sample}", f"{name}=/nonexistent/downpour.txt"]
                    extra = ["Source=7"]
                elif name == "Font File":
                    if font is None:
                        print(f"  skip {name:22s} (no system font found to test with)")
                        continue
                    ends = [f"{name}=", f"{name}={font}"]
                    extra = ["Source=2", "Mutation=0.0"]
                else:
                    print(f"  skip {name:22s} (no sweep defined)")
                    continue
            elif kind == "option":
                # Dropdowns whose 0 and 1 are two adjacent entries; sweep across
                # the whole range instead.
                ends = [f"{name}=0.0", f"{name}=3.0"]
            else:
                ends = [f"{name}=0.0", f"{name}=1.0"]

            before = render(dptest, low, [ends[0]], extra, effect)
            after = render(dptest, high, [ends[1]], extra, effect)
            delta = difference(before, after)
            checked += 1

            if delta < 0.01:
                dead.append((name, delta))
                print(f"  DEAD {name:22s} both ends identical")
            elif args.verbose:
                print(f"  ok   {name:22s} mean delta {delta:.3f}")

    print(f"{checked} parameters swept, {len(dead)} dead")
    if dead:
        print("\nA parameter that changes nothing is usually a uniform name "
              "that does not match\nthe C++, which is silent everywhere else.",
              file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
