# Downpour

> **AI-assisted project.** This codebase was created with [Claude](https://claude.com/claude-code)
> (Anthropic), directed and reviewed by a human author. The rain is verified
> numerically by an offline harness that drives the real plugin class in a
> headless GL context: it compares ~40,000 rendered cells against an independent
> C++ implementation of the same maths, and separately renders a book, reads the
> characters back out of the pixels and reassembles the text (see
> [Status](#status)). It has **never been loaded into Resolume** — only compiled,
> rendered and measured offline. Check it in your own rig before trusting it in a
> show.

Falling columns of characters — digital rain — for [Resolume](https://resolume.com)
Arena and Avenue, as a pair of FFGL plugins.

![Katakana rain on black](docs/hero.png)

<sub>The default: mirrored half-width katakana, green on black, sixty columns.
Rendered by `dptest`, the offline harness.</sub>

## Two plugins

| | |
| --- | --- |
| **Downpour** | A *source*. Draws rain over its own background, on its own layer. |
| **Downpour Over** | An *effect*. Draws the same rain over the incoming clip, so the controls live on the clip rather than a layer above it. |

Both are the same code with one flag different, which is what stops them drifting
apart.

## It reads whatever you give it

The rain does not know the difference between an alphabet and a book. Both are a
list of characters; the only question is whether it picks from them at random or
reads them in order — and the Source control decides that, because the two
combinations it rules out are both traps. Scattering a novel is an expensive way
to make noise, and sequencing a six-character alphabet is a marquee that repeats
every six cells.

| | |
| --- | --- |
| ![Generated code](docs/code.png) | ![Alice in Courier New](docs/text.png) |
| **Code** — a token grammar, not random characters. What makes text read as *code* at rain speed is its punctuation density: brackets, semicolons, arrows, hex literals and camelCase. Prose has almost none of them. | **A document, played out in order.** Consecutive characters down a column, consecutive columns across the frame, advancing by a screenful each time the drops recycle — so the text genuinely streams past rather than being sampled from. |

**Junk** draws from katakana, katakana with digits, hex, binary, ASCII, digits or
latin. **Four public domain works** are built in — *Alice in Wonderland*, Plato's
allegory of the cave, Hamlet's soliloquy, and Descartes on methodical doubt and
the cogito. Or type your own text, or point it at any `.txt` on disk.

## Any font, and it cannot leave you with a blank frame

Pick from the fonts installed on the machine, or point it at any `.ttf`, `.otf`
or `.ttc`. There is also a bitmap face drawn for this project, and it is the
floor rather than a placeholder: **fallback is per character, not per font.**
Choose a font with no katakana and you get your font for the latin and the
built-in face for the katakana, instead of a grid of empty rectangles.

That matters more than it sounds. A composition stores a font *path*, and the
next machine is a different rig in a different country. A plugin whose picture
depends on a path that may not resolve needs somewhere to land, and landing on a
blank frame in front of an audience is not a defensible answer. Downpour also
writes the resolved **family name** into the composition alongside the index, and
prefers the name when the two disagree — names survive the trip between machines;
dropdown positions do not.

## Over your footage

![Rain over a clip](docs/over.png)

<sub>`Downpour Over` on a test gradient, with the plugin's own background at 35%
opacity veiling the clip beneath.</sub>

## Nothing about it drifts

A cell is a **closed-form function of its column, its row and the host's clock**.
There is no simulation state anywhere: no feedback texture, no ping-pong buffer,
no "previous frame".

That is not an implementation detail, it is the feature. A state-based rain
advances one frame at a time, so its speed is whatever the frame rate happens to
be — and Resolume's frame rate drops when the show gets heavy. This one runs at
the speed you asked for whether the host is managing 60 fps or 20, looks
identical at 720p and at 4K, and can be scrubbed to any point without playing
what came before.

## Controls

**Rain** — Speed, Direction (down, up, right, left), Columns, Trail, Density,
Mutation, Fade, Seed.

**Text** — Source, Characters, Custom Text, Text File, Mirror Glyphs.

**Font** — Font, Font File, Glyph Size.

**Colour** — Text colour and opacity, Head colour and boost, Background colour
and opacity, Glow.

**Output** — Mix (the effect variant: how the rain sits over the clip).

The head colour is worth finding early. The bright leading character is most of
what reads as digital rain rather than as a screensaver.

## Download

Grab the latest build from
[Releases](https://github.com/stoatworks-labs/downpour/releases):

- **macOS** — universal, so it loads in both Apple Silicon and Intel builds of
  Resolume.
- **Windows** — installer, or a `.zip` if you would rather place the `.dll`
  yourself.

The macOS build is **not notarised**.

## Status

Verified offline, never in a host. `tools/verify.sh` runs:

| Check | What it proves |
| --- | --- |
| `--rain` | The shader's arithmetic against an independent C++ implementation, over ~40,000 cells: every direction, three resolutions, mutation on and off, and a drop that has wrapped. Worst disagreement ~3e-6. |
| `--readback` | A rendered frame of *Alice* read back character by character out of the pixels and reassembled — **100%**. Covers UTF-8 decode, atlas build, slot mapping, the stream texture and the glyph fetch. |
| `--font` | The drawn face's own invariants, and where every glyph in the atlas came from. |
| `sweep.py` | All 30 controls move the picture. This is the only thing that catches a GLSL uniform whose name does not match the C++, which is silent everywhere else. |
| `lipo` / `nm` | The macOS bundles are universal and export `plugMain`. |

Nothing here has been run in Resolume, on real footage, or against a projector.

## Building

```bash
git clone --recursive https://github.com/stoatworks-labs/downpour
cd downpour
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
cmake --install build     # into ~/Documents/Resolume Arena/Extra Effects
```

`CLAUDE.md` is the command reference. `AGENTS.md` is the design rationale and the
list of traps — read it before changing the rain maths, the atlas or the font
handling.

## Licence

MIT — see [LICENSE](LICENSE).

The glyphs in `source/GlyphData.cpp` are drawn for this project and are MIT along
with everything else. Downpour deliberately ships **no** recreation of the film's
typeface: the well-known fan font is free for personal use only. Mirrored
half-width katakana is a technique rather than something anyone owns, so the
built-in face draws its own, unmirrored at their real code points and flipped at
sample time.

The four built-in passages are public domain in the United States, taken from
Project Gutenberg with the header, footer and trademark stripped — see
[texts/SOURCES.md](texts/SOURCES.md).

Vendored: the [Resolume FFGL SDK](https://github.com/resolume/ffgl) and
[stb_truetype](https://github.com/nothings/stb) (public domain).

This project is not affiliated with, endorsed by, or connected to Warner Bros. or
the makers of *The Matrix*.
