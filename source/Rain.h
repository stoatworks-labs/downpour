#pragma once

#include <array>
#include <cstdint>

/**
    What is in a cell, and when.

    ## The one idea

    **A cell is a pure function of (column, row, time).** There is no simulation
    state anywhere in this plugin: no feedback texture, no ping-pong buffer, no
    "previous frame". Every column's speed, starting phase, trail length and
    place in the document fall out of a hash of the column index, and the head's
    position is arithmetic on the host's clock.

    That is not a micro-optimisation. Three things follow from it, and all three
    are the reason it is written this way:

    - **It cannot drift.** A state-based sim advances by a frame at a time, so
      its speed is whatever the host's frame rate happens to be. Resolume's
      frame rate is not a constant -- it drops when the show gets heavy -- and a
      rain that slows down when the projection load goes up is a rain that
      cannot be cued against anything.
    - **Any frame can be rendered directly.** `dptest` asks for t = 91.7 and
      gets exactly the frame Resolume would draw at 91.7, without playing the
      preceding ninety-one seconds. Nearly every test in the harness depends on
      this.
    - **Resolution independence is free rather than fought for.** The cell at
      (12, 30) is the same cell at 720p and at 4K because nothing about it is
      derived from a pixel.

    ## The mirror

    This file exists **twice**: here, in C++, because it has to be readable and
    testable, and in GLSL in `Shaders.cpp`, because it runs per pixel. Every
    mirrored constant and step carries a `//= mirrored` comment on this side and
    a matching one on the other.

    Change one, change both, and run `dptest --rain`, which is the only thing
    that will notice. The arithmetic is deliberately all `uint32` and float32
    with no doubles and no `sin`-based hashing, precisely so that the two can
    agree exactly rather than approximately -- a `fract(sin(x)*43758.5453)` hash
    gives different answers on different GPUs and would make the comparison
    meaningless.
*/
namespace downpour
{
/// Which way the rain runs. Implemented as a remap of (x, y) onto (column, row)
/// at the very top of Evaluate, so everything after it is written once for a
/// thing that falls downwards.
enum class Direction : int
{
	Down = 0,
	Up,
	Right,
	Left,

	Count
};

/// How a cell picks its character out of the stream.
enum class Flow : int
{
	/// Any character, any time. The junk generator: each cell draws
	/// independently, so the column reads as noise.
	Scatter = 0,

	/// Consecutive characters down a column, consecutive columns across the
	/// screen, advancing by a screenful each time the drops recycle. This is
	/// what makes a loaded document *play out* rather than merely supply
	/// letters.
	Sequence,
};

/// Everything the rain needs to know, in physical units. Controls.cpp converts
/// the host's 0..1 parameters into this; nothing in here is a slider position.
struct RainState
{
	float time    = 0.0f;///< host clock, seconds
	int columns   = 60;
	int rows      = 34;
	float speed   = 8.0f; ///< rows per second, before per-column variation
	float trail   = 0.45f;///< trail length as a fraction of the run
	float density = 0.85f;///< fraction of columns that carry a drop at all
	float mutate  = 8.0f; ///< glyph changes per second, 0 for a still character
	float falloff = 1.6f; ///< brightness exponent along the trail
	uint32_t seed = 1u;

	Direction direction = Direction::Down;
	Flow flow           = Flow::Scatter;

	/// Number of entries in the stream texture -- the alphabet in Scatter, the
	/// document in Sequence.
	int streamLength = 1;

	/// Audio: a per-column brightness gate from the host's FFT. `audio` holds
	/// smoothed spectrum bins, low frequencies first; `audioLevel` is how hard
	/// a column is gated by its band. Zero -- the default, and all an OFX host
	/// can provide -- leaves the rain exactly as it was.
	float audioLevel = 0.0f;
	std::array< float, 64 > audio = {};
};

/// What one cell resolved to.
struct Cell
{
	/// 0 where nothing is lit, rising to 1 at the head.
	float brightness = 0.0f;

	/// 1 exactly at the leading character, falling to 0 within about a row and
	/// a half. What the head colour is mixed in by, and most of what reads as
	/// "digital rain" rather than "scrolling text".
	float head = 0.0f;

	/// Index into the stream. Meaningless where brightness is 0.
	int stream = 0;
};

//-------------------------------------------------------------------------
// The hash. Chris Wellons' `lowbias32`: two multiplies and three xorshifts,
// with the best measured avalanche of any 32-bit integer hash of that size.
//
// An integer hash and not the `fract(sin(x) * 43758.5453)` that shader code
// usually reaches for, because that one is transcendental and its result
// differs between GPUs, between drivers, and between the GPU and the CPU.
// This one is exact everywhere, which is what makes the C++ and the GLSL
// comparable at all.
//-------------------------------------------------------------------------
uint32_t Hash( uint32_t x );                    //= mirrored
uint32_t HashCombine( uint32_t seed, uint32_t v );//= mirrored

/// A hash's top 24 bits as a float in [0, 1).
///
/// Twenty-four and not thirty-two: float32 has a 24-bit significand, so this is
/// the widest slice that converts without rounding. Feeding the full 32 bits in
/// would round, and round differently on each side of the mirror.
float Unit( uint32_t h );//= mirrored

/// Resolve one cell. `x` and `y` are grid coordinates with the origin at the
/// **top left**, which is where the shader's are too.
Cell Evaluate( const RainState& state, int x, int y );//= mirrored

/// The head's position, in rows, for one column at one time -- exposed so the
/// harness can check the rendered picture against the commanded speed rather
/// than against a second copy of its own belief. `cycle` is written out too
/// because "how far has it got" is meaningless without "out of what".
float HeadPosition( const RainState& state, int column, float& cycle, int& drop );//= mirrored

} // namespace downpour
