# downpour — orientation for another LLM (or a newcomer)

**What it is:** digital rain — falling columns of characters — as **two** FFGL
2.1 plugins for Resolume Arena/Avenue. `Downpour` is a source that draws over its
own background; `Downpour Over` is an effect that draws the same rain over the
incoming clip. C++17 + GLSL 4.1, CMake, universal macOS `.bundle` and a Windows
`.dll`. Public, MIT, `github.com/stoatworks-labs/downpour`.

`CLAUDE.md` is the command reference — build, install, verify. This file is the
*why*: read it before touching the rain maths, the glyph atlas, or the font
handling.

---

## The one idea

**A cell is a pure function of (column, row, host time).**

There is no simulation state anywhere: no feedback texture, no ping-pong buffer,
no "previous frame". Every column's speed, phase, trail length and place in the
document falls out of a hash of the column index, and the head's position is
arithmetic on the host's clock.

Three things follow, and all three are the reason it is written this way:

- **It cannot drift.** A state-based rain advances a frame at a time, so its
  speed is whatever the host's frame rate happens to be — and Resolume's frame
  rate drops when the show gets heavy. A rain that slows down when the projection
  load goes up cannot be cued against anything.
- **Any frame can be rendered on its own.** `dptest` asks for t = 91.7 and gets
  exactly the frame Resolume would draw at 91.7, without playing the preceding
  ninety-one seconds. Nearly every test depends on this.
- **Resolution independence is free rather than fought for.** The cell at (12,
  30) is the same cell at 720p and at 4K because nothing about it comes from a
  pixel. `--rain` asserts this across three resolutions.

### What falls out of it

- **One pass.** asciify needs three because its cell pass *averages the incoming
  picture*, and a reduction has to happen at its own resolution. Downpour reduces
  nothing, so every pixel works out which cell it is in and asks. If this ever
  seems slow, adding a cell buffer is the wrong reach — there is nothing to cache
  in it that is not two hashes and a multiply away.
- **The stream is one thing.** A character set and a loaded book are both just a
  list of code points; `Flow::Scatter` indexes it by hash and `Flow::Sequence`
  indexes it consecutively. One texture, one length uniform, one place where a
  code point becomes an atlas slot.

---

## The traps

Ordered by how much time they will cost you.

**`FFGLShader` has no integer-vector setter, and using the float one is
silent.** `Set( const char*, float, float )` against an `ivec2` uniform is a
`glUniform2f` on an integer uniform — a `GL_INVALID_OPERATION` that leaves the
value at zero, with nothing anywhere the plugin can see. `StreamSize` was
declared `ivec2`, every glyph resolved to the blank slot, and the picture came
out as faint mip-averaged smudges that looked like a sampling bug. **Declare
`vec2` and convert in GLSL.** The overloads that exist are `float`, `vec2`,
`vec3`, `vec4` and `int` — anything else needs a raw `glUniform*` (which is what
`Seed`, a `uint`, uses).

**The grid runs y-down and a GL texture runs y-up.** `local.y` comes from the
cell grid, whose origin is top left because that is how falling rain is
described and how `Rain.cpp` is written. `SampleGlyph` flips it, and that is the
**only** place either convention may change. Without the flip every glyph renders
upside down — which is close to invisible on mirrored katakana and reads as
merely an odd typeface on lower case. `--readback` scored 34% and nothing else
noticed.

**`tools/sweep.py` is the only thing that catches a dead control.** A GLSL
uniform name that does not match the C++ is silently ignored:
`glGetUniformLocation` returns −1 and `glUniform` on −1 is a documented no-op, so
a slider is stone dead while everything compiles, links, loads and renders.
`--rain` will not catch it — it checks that the GPU and the C++ agree, and an
unset uniform makes them agree perfectly on the wrong thing.

**The head's precision degrades with the clock, and that is inherent.** `head` is
`travel − cycle * drop`: two large nearly-equal numbers subtracted to leave a
small one, and `travel` grows without bound. At two and a half minutes and twenty
rows a second it is already ~4700, so float32 leaves about 3e-4 of a row. A
ten-hour show reaches about a hundredth of a cell. **`--rain`'s tolerance scales
with `time * speed` for exactly this reason** — a flat tolerance loose enough for
the worst case would also cover a genuine divergence in every short case.

**The rain maths exists twice.** In C++ in `Rain.cpp`, because it has to be
readable and testable, and in GLSL in `Shaders.cpp`, because it runs per pixel.
Every mirrored constant and step carries a `//= mirrored` comment on both sides.
Change one, change both, and run `--rain`.

**Use an integer hash, never `fract(sin(x) * 43758.5453)`.** That one is
transcendental: its result differs between GPUs, between drivers, and between the
GPU and the CPU, which would make the mirror meaningless. `lowbias32` is exact
everywhere. For the same reason `Unit()` takes the **top 24 bits** — that is the
widest slice that converts to float32 without rounding, and rounding would round
differently on each side.

**A mutation tick can land on a float boundary.** `floor( time * mutate + phase )`
after minutes at twenty-odd changes a second is a four-digit float32, and the GPU
may fold the multiply-add into an FMA where the CPU does not. Two cells in four
thousand land on opposite sides and hash to unrelated glyphs. `--rain` exempts
cells within 1e-3 of a tick boundary from the *glyph* comparison only, counts
them, and fails if they ever exceed 1% of the frame.

**A ranged parameter cannot have a ranged default.** `SetParamInfo` clamps an
`FF_TYPE_STANDARD` default into 0..1 *before* returning, and `SetParamRange` can
only be called afterwards. There is no `SetParamDefault`. So every host parameter
here is 0..1 and the conversions live in `Controls.cpp`. A default of 8 rows per
second would silently become 1.

**The plugin registers itself from a file-scope constructor.** `CFFGLPluginInfo`
is never referenced by name, so in a **STATIC** archive the linker may drop the
whole translation unit — giving a bundle that loads, exports `plugMain`, and
reports that it contains no plugins. `downpour_core` is an **OBJECT** library,
and `SourcePlugin.cpp` / `EffectPlugin.cpp` are listed **directly** in their own
`MODULE` targets. Putting either in the shared library would register both
plugins into both bundles. `tools/verify.sh` checks the export with `nm`.

**A font chosen from the dropdown is stored as an index into a list that is
specific to the machine it was chosen on.** Open the composition on a rig with a
different set of fonts and index 47 is a different typeface. `GetTextParameter`
returns the resolved *family name*, so the host serialises a portable name at no
cost to the operator, and `ResolveTypeface` prefers it over the index when the
two disagree — except immediately after the operator moves the dropdown, which is
what `fontIndexChosen` is for.

**Font fallback is per codepoint, not per font.** Pick a font with no katakana
and you get that font for the latin and the built-in face for the katakana. This
is the thing that makes "set the font" safe to offer at all; without it the
picture is a grid of `.notdef` boxes. `--font` reports the split.

**Scale a rasterised glyph by ascent−descent, not by the em.** An em box is
nominal and most faces overflow it, so `stbtt_ScaleForPixelHeight` clips the tops
off capitals in about a third of the fonts on this machine.

**Font family names are not always Latin.** A font that ships an Arabic or
Chinese family name usually ships an English one too; taking whichever record
came first put a run of families called `????` at the top of the dropdown. Prefer
name ID 16 over 1, English language records over others, and reject a decode that
is more than a quarter `?`. Also skip families beginning with `.` — macOS has
about a hundred internal faces that are UI machinery rather than typefaces.

**MSVC rejects a string literal over 65535 bytes.** A hard error, and one that
appears only on the Windows build — which is to say only after macOS has already
gone green. `source/Corpus.cpp` is generated as **byte arrays**.

**The atlas border is load-bearing.** One blank texel on every side, and the
fetch insets half a texel further. `GL_LINEAR` at the very edge of a glyph body
otherwise takes half its weight from the neighbouring slot.

**Divide by the atlas texture's own size, not the area the slots cover.** 31
slots of 66 texels is 2046 of 2048; dividing by 2046 is invisible mid-stroke and
shows as a sliver of the next slot along the far edge.

**The stream texture must be read with `texelFetch`, never `texture`.** Its
values are atlas slot numbers, not quantities. Interpolate two and you address a
third glyph that neither cell chose.

**`flat`, `active`, `filter`, `input`, `output`, `sample` and `common` are GLSL
reserved words**, and a shader that fails to compile surfaces only at runtime, as
"the plugin does nothing". That is what `Diag` is for.

---

## Checking your work

`tools/verify.sh` runs the lot. The two that matter check different halves:

- **`--rain`** compares the shader's arithmetic against `Rain.cpp` across eight
  cases — every direction, three resolutions, mutation on and off, and a drop
  that has wrapped. ~40,000 cells. It would catch a constant changed on one side
  only. It never looks at a glyph.
- **`--readback`** renders Alice at one output pixel per atlas texel, identifies
  each character by correlating the cell against the atlas, and reassembles the
  string. That covers UTF-8 decode, atlas build, slot mapping, the stream texture
  and the glyph fetch — and it is the only test that would notice the atlas being
  indexed off by one. It demands **100%**: correlation against a known atlas at
  native size is not a fuzzy problem, and an occasional wrong answer would mean
  two glyphs are genuinely identical, which is worth knowing rather than
  tolerating.

`--font` also asserts the drawn face's own invariants — every row exactly twelve
cells of `#` or `.`, no codepoint drawn twice. A typo there does not fail to
compile; it silently shortens a stroke.

**Host verification is Allan's, not an agent's.** Driving the Resolume GUI from a
session is unreliable. Nothing in this repo has been loaded into Resolume yet.

---

## Things deliberately not done

- **No blur for Glow.** It would need a second pass and a buffer; at rain speed
  what reads as glow is mostly the trail refusing to go fully dark, which is a
  lift on the trail's own brightness.
- **No bundled Matrix font.** The well-known fan recreation is free for personal
  use only. The technique — mirrored half-width katakana — is not something
  anyone owns, so `GlyphData.cpp` draws its own, and they are MIT with the rest.
  They are drawn **unmirrored** at their real codepoints and flipped at sample
  time, so the atlas dump stays checkable against a real font.
- **Not named after the film.** "The Matrix" is a Warner Bros trademark and this
  ships as a public product. "Digital rain" is the generic term.
- **Excerpts, not complete works.** At any legible speed a minute of rain gets
  through a chapter; nobody was ever going to reach the end of the *Republic*.

Related: [asciify](https://github.com/stoatworks-labs/asciify) (the glyph atlas,
harness and CMake patterns came from there), old-cathode, porthole,
resolume-luma-keyer.


## The browser demo

`demo/` is a static page at **downpour-demo.stoatworks-labs.com**: this
plugin's own GLSL, ported to WebGL2, running on clips generated in the page with
the parameters the constructor declares. It is deployed as a Cloudflare Worker
serving `demo/` as static assets (`wrangler.toml`), with **no build step** — what
is committed is what is served.

Three things about it are not visible from the files:

- **`demo/plugin.js` carries a second copy of the shader.** The demo cannot
  include a C++ file, so the GLSL from `source/Shaders.cpp` is duplicated there and
  *nothing enforces that they agree*. Change the shader and change both, or the
  page quietly goes on rendering the old maths.
- **`demo/vendor/` is vendored, not authored here.** The master is
  `stoatworks-backend/resolume-demo/`; fix it there and re-run its `sync.sh`.
  `sync.sh --check` reports drift. A fix applied to the copy fixes one plugin out
  of six.
- **Verify a deploy by content, never by status code.** A wrong page still
  answers 200.

```bash
cf-run npx wrangler deploy
curl -s 'https://downpour-demo.stoatworks-labs.com/?cb=1' | grep -o '<title>[^<]*'
```

`demo/data.js` is generated from `source/GlyphData.cpp` and `texts/` by
`demo/tools/extract_data.py` (`--check` reports staleness). Note the trap
that script documents: the glyph array keys are **both** hex codepoints
and character literals, and matching only the hex spelling yields 49 of
the 140 glyphs — an atlas missing its entire ASCII half, which reads as a
broken character set rather than a broken parser.

The page is emphatic that it is not the plugin, and lists what it does not
reproduce in a disclosure at the foot. Keep that: it is a port, so nothing on it
is evidence about the plugin, and the offline harness in this repository is
still the only thing that measures anything.

## Factory presets

### The host owns the parameters, and a preset had to learn that

Reported against **vertigo** as its issue #2 and fixed across all seven plugins
on 2026-08-22: choosing a factory preset in Resolume did nothing and the
dropdown snapped straight back to `Custom`.

The pattern was copy-based — `applyPreset` writes the values into `params[]` and
raises `FF_EVENT_FLAG_VALUE` so the host re-reads its sliders — and it rests on
an assumption FFGL never makes. **The host owns parameter state.** It pushes its
own values back down whenever it likes, and nothing obliges it to act on a value
event. Resolume does not: it carries on restating the values it still believes
in, which are the ones from before the preset. Those restatements arrive as
`SetFloatParameter` calls carrying a changed value, so the rule "a covered
parameter changed, therefore the operator has taken over" fired on the host's
own echo, instantly, every time.

Three things now arrive through that one call while a preset is active, and only
the third is a person:

| What arrives | How it is recognised | What happens |
|---|---|---|
| the preset's own values, from a host that honoured the events | matches the preset | ignored — nothing to write |
| the values from *before* the preset, from a host that did not | matches `hostValues[]`, the host's own last word | ignored — writing it would undo the preset |
| a new value from neither | matches neither | written, and the preset falls back to Custom |

`hostValues[]` is the record of what the **host** last sent, which is not what
the plugin is rendering with, and `seedHostValues()` fills it from the defaults
on the first parameter traffic — **before `applyPreset` can run**. Seeding it
afterwards records the preset's own values as the host's opening position, so
the host's very next restatement looks like an edit; that mistake was made once
during the fix and the test caught it.

Two tolerances matter and they are not the same number. `kSame` is **1e-3**, a
host-quantisation allowance rather than a float epsilon — a host that keeps its
parameters shorter than a float hands back a number *near* ours. The pre-existing
"did a covered parameter move?" test below still works to 1e-4, which is why a
value matching the preset is **ignored rather than written**: letting a rounded
copy of our own value into `params[]` would trip that tighter test.

`dptest --presets` drives all three hosts across every preset, with no GL
involved, and runs in `tools/verify.sh`. Against the pre-fix code it fails in
exactly the "ignores value events" column.

`source/Presets.h` is one table of named looks in the host-facing 0..1
parameter space, and it drives **both** builds — the FFGL constructor and the
OFX describe each read it, so a preset cannot drift between Resolume and
Resolve. Element 0 of the dropdown is always **Custom**, which is not in the
table: it means "the sliders are the truth".

The mechanics are deliberately copy-based. Picking a preset copies the table
row into the real parameters — the FFGL side raises `FF_EVENT_FLAG_VALUE` per
changed parameter so the host re-reads its sliders, the OFX side setValues
inside one edit block so undo takes the whole preset back at once. A host that
ignores the events still renders the preset correctly and merely shows stale
knobs. Editing any covered parameter afterwards flips the dropdown back to
Custom — judged by comparing values, not by the change reason, so a host
echoing our own writes cannot un-set the preset.

A preset covers rain, text selection and colour. It stays off Custom Text,
Text File and the Font choice (a system font index means something different
on every machine), the Seed, and Mix. Both FFGL plugins share DownpourPlugin,
so the source and the effect get the same dropdown.

Verified by rendering a preset and its hand-set equivalent through the offline
harness and `ofxprobe --edit` (which delivers the set as a real user edit,
with `kOfxActionInstanceChanged`) and comparing byte-for-byte.
