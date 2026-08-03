# downpour

Digital rain — falling columns of characters — as **two** FFGL plugins for
Resolume Arena/Avenue: a source (`Downpour`) and an effect that draws over the
clip (`Downpour Over`). C++/GLSL, CMake MODULE → universal `.bundle` (macOS) +
Windows `.dll`. Public MIT repo.

Read `AGENTS.md` before changing the rain maths, the glyph atlas or the font
handling.

## Commands (CMake)
- Configure: `cmake -B build -DCMAKE_BUILD_TYPE=Release`
- Fast dev build: add `-DCMAKE_OSX_ARCHITECTURES=arm64`
- Build: `cmake --build build`
- Install both bundles to Resolume: `cmake --install build`
- Render a frame offline: `./build/dptest --out /tmp/frame.png --time 12.5`
- The effect variant over a test clip: `./build/dptest --effect --out /tmp/over.png`
- List parameters: `./build/dptest --list`
- Look at the glyph atlas: `./build/dptest --atlas /tmp/atlas.png`
- Set anything by name: `--set "Columns=0.45" --set "Source=2"`

## OpenFX build
- `source/ofx/DownpourOFX.cpp` → `build/Downpour.ofx.bundle` (target `DownpourOFX`,
  `-DBUILD_OFX=OFF` to skip): **both** plugins in one bundle — `com.stoatworks.downpour`
  (generator) and `com.stoatworks.downpourover` (filter) — for Resolve/Nuke/Natron/Vegas.
- Rain/stream/atlas/typeface code linked straight from source; only the fragment
  shader's per-pixel machinery (glyph fit, atlas sampling, composite) is mirrored.
- OFX time arrives in *frames*; the plugin divides by the clip frame rate to get
  the seconds Rain.cpp wants.
- Smoke test (ofxprobe only drives the Filter context, so the generator's
  describe is checked but its render runs only in a real host):
  `../resolume-ofx-bridge/build/ofxprobe --dir build --render com.stoatworks.downpourover --size 640x360 --out /tmp/d.bmp`
- OFX SDK subset (BSD-3) vendored under `external/openfx`.
- Install for Resolve: copy the bundle into `/Library/OFX/Plugins`.

## Verify
- Everything: `tools/verify.sh`
- Shader vs `Rain.cpp`: `./build/dptest --rain`
- A document through the atlas and back: `./build/dptest --readback`
- The face's invariants and where glyphs came from: `./build/dptest --font`
- No dead controls: `python3 tools/sweep.py`

## Regenerating the built-in texts
- `tools/extract_texts.py --source DIR` cuts the four passages out of their
  Project Gutenberg sources into `texts/`. Run by hand; the output is committed.
- `tools/embed_texts.py` turns `texts/*.txt` into `source/Corpus.cpp`.
- Both have a `--check` mode, and `verify.sh` runs them, so the committed files
  cannot drift from their sources.

## Notes
- **One pass.** A cell is a closed-form function of (column, row, time) — no
  simulation state, no feedback buffer. That is what makes it frame-rate
  independent, resolution independent, and testable a frame at a time.
- The rain maths exists twice, in `Rain.cpp` and in the shader in `Shaders.cpp`.
  Every mirrored line is marked `//= mirrored`. `--rain` measures one against the
  other. Change one, change both, run it.
- **`FFGLShader::Set` has no integer-vector overload**, and using the float one
  on an `ivec2` silently leaves the uniform at zero. Declare `vec2` in GLSL.
- The cell grid runs y-down; a GL texture runs y-up. `SampleGlyph` is the only
  place that flip happens.
- All host parameters are 0..1 and mapped in `Controls.cpp`. `SetParamInfo`
  clamps a standard default into 0..1 before `SetParamRange` can widen it.
- The stream texture must be read with `texelFetch`. Its values are glyph slot
  numbers, not quantities.
- `downpour_core` is an **OBJECT** library, and each plugin's registration is
  listed directly in its own target — see `AGENTS.md`.
- macOS build must be universal (arm64 + x86_64). Verify with `lipo`, never the
  build log.
- `flat`, `active`, `filter`, `input`, `output`, `sample`, `common` are GLSL
  reserved words. Shader errors surface only at runtime, in the diagnostics log.
- Public repo. "Commit" = commit **and** push.

## Diagnostics

`source/Diag.{h,cpp}` — log file only, no crash handler (this runs inside
Resolume), no bundle command. It covers the three failures that actually happen
and all look identical from outside ("it does nothing"): a shader that will not
compile, a font that will not load or has no glyph for anything asked of it, and
a text file that will not open.

    ~/Library/Logs/downpour/downpour.YYYY-MM-DD.log
