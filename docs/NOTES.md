# Notes

Working notes for this repo: status, decisions, and the traps that have actually bitten.
Migrated out of Claude Code's memory on 2026-08-24, so they are written in the first
person and dated by when each thing was learned — that date is usually the useful part.

Cross-cutting notes that are not specific to this repo live in
[fleet-notes](https://github.com/stoatworks-labs/fleet-notes).

*downpour — digital rain (Matrix-style falling code) as TWO FFGL plugins for Resolume, a source and an effect; PUBLIC MIT v0.1.0 released, site + video live, verified offline only and never loaded into Resolume*

**downpour** (started 2026-08-03) — **digital rain** for Resolume Arena/Avenue:
falling columns of characters. `~/Projects/downpour`, **MIT**.

**PUBLIC and RELEASED as of 2026-08-03**: `github.com/stoatworks-labs/downpour`,
**v0.1.0** with macOS universal .dmg/.zip and Windows setup.exe/.zip built by CI.
Website page live at `stoatworks-labs.com/software/downpour/` and in the Resolume
suite; video live at **youtube.com/watch?v=Sdvmpz_GiTo**, both embed links wired.
**Still never loaded into Resolume** — verified entirely by its own offline
harness, and the README and statusNote both say so.

**The Windows build needs `NOMINMAX`.** `windows.h` defines `min`/`max` as macros,
so `std::min` in `Typeface::BuildAtlas` became `std::(...)` and failed with
`C2589: '(': illegal token on right side of '::'` — an error naming neither `min`
nor `windows.h`, reachable only in CI after macOS had already gone green.

**Instagram Reel LIVE** at instagram.com/reel/DblAwrKDkCx/ (2026-08-03).

**It now has a `stoatworks-backend/video/projects/downpour/` entry, and did not
before.** The video was cut outside the toolkit and the frames went to
`/tmp/dp-frames`, so when a Reel was wanted the upload existed and every
intermediate was gone — nothing local to cut from, and nothing in the toolkit's
git history either. `render.py` there now re-runs downpour's own cue sheet into
`work/frames` and encodes it, so the cue sheet stays the source of truth. **The
beats in it are new** (the YouTube cut's were never committed): same footage,
same times, read off the cue sheet's own section comments, agreed as not needing
to be frame-accurate.

**`dptest --sequence DIR --script tools/video.cues`** renders the project video:
one process, cue sheet drives the real parameters (`T Name=V`, or
`T1..T2 Name=V1..V2` for an eased ramp). 1350 frames of 1080p in 40 seconds.

**Two bundles from one class**, plugin IDs `DP01` and `DP02`: `Downpour`
(FF_SOURCE, own background) and `Downpour Over` (FF_EFFECT, over the clip). They
differ by a constructor flag and a `#define` handed to the shader compiler.
`downpour_core` is an OBJECT library; each registration TU is listed **directly**
in its own MODULE target, or both plugins register into both bundles.

**The one idea: a cell is a closed-form function of (column, row, host time).**
No simulation state, no feedback buffer. So it cannot drift with the host frame
rate, is resolution independent for free, and **any frame can be rendered on its
own** — which is what nearly every test depends on. It also collapses to **one
pass**: asciify needs three because its cell pass *averages the input*, and
downpour has no input to reduce.

**Font handling is the part worth remembering.** Fonts are found by walking the
OS font directories and reading each file's `name` table directly (one code path
on macOS and Windows, no CoreText/DirectWrite, and the scan yields the *path* the
rasteriser needs anyway). Rasterised with vendored **stb_truetype** (public
domain). **Fallback is per codepoint, not per font** — pick a font with no
katakana and you get that font for the latin and the hand-drawn bitmap face for
the katakana, which is what makes "set the font" safe to offer. `GetTextParameter`
returns the resolved **family name** so the host serialises something portable:
the dropdown index means a different typeface on a machine with different fonts
installed.

**Built-in face:** 140 glyphs, 12×12, drawn in `GlyphData.cpp` as ASCII art
(asciify's `FontData.cpp` convention). Katakana are drawn **unmirrored at their
real codepoints** and flipped at sample time, so the atlas dump stays checkable
against a real font. **Ships no recreation of the film's typeface** — Matrix Code
NFI is free for personal use only. Not named after the film either (Warner Bros
trademark; "digital rain" is the generic term).

**Four public domain passages embedded** as byte arrays (MSVC caps a string
literal at 65535 bytes): Alice ch.I, Plato's cave, Hamlet III.i, and — note the
substitution — **Descartes' *Discourse on the Method* part IV, NOT the
*Meditations***. The only English *Meditations* in the public domain is
Molyneux's 1680 translation, which is period spelling and heavy italic markup and
renders as noise. The *Discourse* carries the same three arguments (methodical
doubt, the dream argument, the cogito) in Veitch's clean 1850 prose.
`tools/extract_texts.py` + `tools/embed_texts.py` regenerate, both with
`--check`, both run by `verify.sh`, so the committed files cannot drift.

**Verification** (`tools/verify.sh`, all green): `--rain` compares the shader
against the C++ mirror in `Rain.cpp` over ~40k cells (worst disagreement 3e-6);
`--readback` renders Alice and reads the characters back **out of the pixels**,
demanding 100%; `--font` checks the drawn face's invariants; `tools/sweep.py`
proves all 30 controls move the picture. **`--readback` is the one that earns its
keep** — it caught glyphs rendering upside down, which `--rain` cannot see
because it never looks at a glyph.

Traps live in the repo's AGENTS.md — see [agents md convention](https://github.com/stoatworks-labs/fleet-notes/blob/main/notes/reference_agents_md_convention.md) and
[ffgl sdk bugs](https://github.com/stoatworks-labs/fleet-notes/blob/main/notes/reference_ffgl_sdk_bugs.md). Related: [resolume luma keyer](https://github.com/stoatworks-labs/resolume-luma-keyer/blob/main/docs/NOTES.md) (`resolume-luma-keyer`),
[old cathode](https://github.com/stoatworks-labs/old-cathode/blob/main/docs/NOTES.md) (`old-cathode`), [porthole](https://github.com/stoatworks-labs/porthole/blob/main/docs/NOTES.md) (`porthole`), [resolume scopes](https://github.com/stoatworks-labs/resolume-scopes/blob/main/docs/NOTES.md) (`resolume-scopes`),
**disclaimer scope** (working-practice note, kept in Claude memory) (AI disclaimer applies).
