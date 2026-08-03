#pragma once

/**
    One pass.

    asciify needs three because its cell pass *averages the incoming picture*,
    and averaging a few million pixels down to a few thousand cells is a
    reduction that has to happen at its own resolution. Downpour reduces
    nothing: a cell is a closed-form function of its own coordinates and the
    clock (see Rain.h), so every pixel can work out on its own which cell it is
    in, ask that question, and answer it. One fragment shader over one quad.

    That is worth stating because "add a cell buffer" is the obvious thing to
    reach for if this ever gets slow, and it would be the wrong reach: there is
    nothing to cache in it that is not two hashes and a multiply away.

    ## The mirror

    `kRainShader` contains a line-for-line reimplementation of `Rain.cpp`. Every
    mirrored line is marked `//= mirrored` on both sides. `dptest --rain` renders
    a frame and checks the result against the C++, and it is the only thing that
    will notice them drifting apart.

    ## Things that will catch you out

    - **A uniform name that does not match the C++ is silently ignored.**
      `glGetUniformLocation` returns -1 and `glUniform` on -1 is a documented
      no-op, so a control is simply dead while everything compiles, links, loads
      and renders. `--rain` will not catch it: it checks that the GPU and the
      C++ agree, and a dead uniform makes them agree perfectly on the wrong
      thing. `tools/sweep.py` is what catches it.

    - **`flat`, `active`, `filter`, `input`, `output`, `sample` and `common` are
      GLSL reserved words.** A shader that will not compile surfaces only at
      runtime, as "the plugin does nothing", which is why Diag exists.

    - **The stream texture must be read with `texelFetch`, never `texture`.**
      Its values are atlas slot numbers, not quantities. Interpolate two of them
      and you address a third glyph that neither cell asked for.
*/
namespace downpour
{
/// Shared by both plugins: passes UV through in 0..1 frame space.
extern const char* const kVertexShader;

/// The rain. Compiled twice -- once as-is for the source plugin, and once with
/// `kEffectDefine` prepended for the effect, which adds the incoming clip.
extern const char* const kRainShader;

/// Prepended to kRainShader to build the effect variant. It goes in after the
/// `#version` line, which is why this is a define rather than a second copy of
/// the shader.
extern const char* const kEffectDefine;

/// Prepended by the harness to make the shader write raw cell state to a float
/// target instead of a picture, so `--rain` can compare it against `Rain.cpp`
/// exactly rather than inferring brightness back out of a composited colour.
/// Never set by either shipping plugin.
extern const char* const kCellDebugDefine;

} // namespace downpour
