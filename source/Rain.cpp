#include "Rain.h"

#include <algorithm>
#include <cmath>

namespace downpour
{
namespace
{
/// Per-column salts. Distinct constants rather than 1, 2, 3 so that two columns
/// whose indices differ by one do not end up with correlated draws -- the hash
/// is good enough that it would probably be fine either way, but "probably
/// fine" is not visible until a wall of rain has a diagonal pattern in it.
constexpr uint32_t kSaltSpeed   = 0x9E3779B9u;//= mirrored
constexpr uint32_t kSaltPhase   = 0x85EBCA6Bu;//= mirrored
constexpr uint32_t kSaltTrail   = 0xC2B2AE35u;//= mirrored
constexpr uint32_t kSaltActive  = 0x27D4EB2Fu;//= mirrored
constexpr uint32_t kSaltGlyph   = 0x165667B1u;//= mirrored
constexpr uint32_t kSaltMutate  = 0xD3A2646Cu;//= mirrored

/// A column runs between 45% and 100% of the commanded speed. Enough spread to
/// break up the horizontal banding you get when every drop falls at one rate,
/// not so much that the slow columns read as a separate, lazier effect.
constexpr float kSpeedMin = 0.45f;//= mirrored

/// And between 35% and 100% of the commanded trail length.
constexpr float kTrailMin = 0.35f;//= mirrored

/// How far past the bottom a drop keeps going before it restarts, as a fraction
/// of the run. Without it every column would restart the instant its head left
/// the frame, and the screen would never have a gap in it -- the gaps are what
/// makes it rain rather than pour.
constexpr float kTailGap = 0.6f;//= mirrored

/// The head fades over this many rows. Under one row and the head flickers as
/// it crosses a cell boundary; much over two and it stops being a head.
constexpr float kHeadRows = 1.5f;//= mirrored

float Mix( float a, float b, float t )
{
	return a + ( b - a ) * t;
}

float Saturate( float v )
{
	return v < 0.0f ? 0.0f : ( v > 1.0f ? 1.0f : v );
}

/// GLSL's smoothstep, written out so the mirror is exact.
float SmoothStep( float edge0, float edge1, float x )//= mirrored
{
	const float t = Saturate( ( x - edge0 ) / ( edge1 - edge0 ) );
	return t * t * ( 3.0f - 2.0f * t );
}

/// GLSL's floor-based mod, which is not C's fmod for negative inputs. Written
/// out because the difference only shows up when a drop wraps, which is exactly
/// where a mismatch would be least obvious.
float FloorMod( float a, float b )//= mirrored
{
	return a - b * std::floor( a / b );
}

int WrapIndex( long long value, int length )//= mirrored
{
	if( length <= 0 )
		return 0;
	long long wrapped = value % length;
	if( wrapped < 0 )
		wrapped += length;
	return static_cast< int >( wrapped );
}
} // namespace

uint32_t Hash( uint32_t x )//= mirrored
{
	x ^= x >> 16;
	x *= 0x7FEB352Du;
	x ^= x >> 15;
	x *= 0x846CA68Bu;
	x ^= x >> 16;
	return x;
}

uint32_t HashCombine( uint32_t seed, uint32_t v )//= mirrored
{
	return Hash( seed ^ ( v * 0x9E3779B9u ) );
}

float Unit( uint32_t h )//= mirrored
{
	// 2^-24. See the header: the top 24 bits are the widest slice that survives
	// the conversion to float32 unrounded.
	return static_cast< float >( h >> 8 ) * ( 1.0f / 16777216.0f );
}

float HeadPosition( const RainState& state, int column, float& cycle, int& drop )//= mirrored
{
	// The run length is however many cells the drop crosses, which depends on
	// the direction: down and up run the height, left and right the width.
	const bool horizontal = state.direction == Direction::Right || state.direction == Direction::Left;
	const float run       = static_cast< float >( horizontal ? state.columns : state.rows );

	const uint32_t columnSeed = HashCombine( state.seed, static_cast< uint32_t >( column ) );

	const float speedScale = Mix( kSpeedMin, 1.0f, Unit( HashCombine( columnSeed, kSaltSpeed ) ) );
	const float phase      = Unit( HashCombine( columnSeed, kSaltPhase ) );

	cycle = run * ( 1.0f + kTailGap );

	// Travel is unbounded and grows with the host clock; the head is where that
	// lands inside the current cycle, and `drop` counts how many whole cycles
	// have gone by. `drop` is what gives a column a fresh place in the document
	// every time it restarts, so it has to be derived here rather than guessed
	// at from the head.
	const float travel = state.time * state.speed * speedScale + phase * cycle;

	drop = static_cast< int >( std::floor( travel / cycle ) );
	return FloorMod( travel, cycle );
}

Cell Evaluate( const RainState& state, int x, int y )//= mirrored
{
	Cell cell;

	if( state.columns <= 0 || state.rows <= 0 )
		return cell;

	//---------------------------------------------------------------------
	// Direction, as a remap onto a thing that falls downwards. Everything
	// below this point is written once, for Down.
	//---------------------------------------------------------------------
	int column = 0;
	int row    = 0;
	switch( state.direction )
	{
	case Direction::Down:  column = x; row = y;                        break;
	case Direction::Up:    column = x; row = state.rows - 1 - y;       break;
	case Direction::Right: column = y; row = x;                        break;
	case Direction::Left:  column = y; row = state.columns - 1 - x;    break;
	default:               column = x; row = y;                        break;
	}

	float cycle = 0.0f;
	int drop    = 0;
	const float head = HeadPosition( state, column, cycle, drop );

	const uint32_t columnSeed = HashCombine( state.seed, static_cast< uint32_t >( column ) );

	// A column that loses the density draw carries no drop at all. Checked
	// against `density` rather than being scaled by it so that turning Density
	// down empties columns rather than dimming all of them, which is what the
	// control is for.
	const float activeDraw = Unit( HashCombine( columnSeed, kSaltActive ) );
	if( activeDraw >= state.density )
		return cell;

	const bool horizontal = state.direction == Direction::Right || state.direction == Direction::Left;
	const float run       = static_cast< float >( horizontal ? state.columns : state.rows );

	const float trailScale = Mix( kTrailMin, 1.0f, Unit( HashCombine( columnSeed, kSaltTrail ) ) );
	const float trailRows  = std::max( 1.0f, state.trail * trailScale * run );

	//---------------------------------------------------------------------
	// Where this row sits behind the head.
	//---------------------------------------------------------------------
	const float distance = head - static_cast< float >( row );

	// Ahead of the head, or past the end of the trail: nothing here. Note the
	// asymmetry -- the head has a hard leading edge and a soft tail, which is
	// what a phosphor does and what the film's rain does.
	if( distance < 0.0f || distance > trailRows )
		return cell;

	const float along = distance / trailRows;
	cell.brightness   = std::pow( Saturate( 1.0f - along ), state.falloff );
	cell.head         = 1.0f - SmoothStep( 0.0f, kHeadRows, distance );

	//---------------------------------------------------------------------
	// Which character.
	//---------------------------------------------------------------------
	if( state.flow == Flow::Sequence )
	{
		// Consecutive down a column, consecutive across columns, advancing by a
		// whole screenful each time the drops recycle. So a document is read in
		// order rather than sampled from -- see Flow::Sequence.
		const long long perScreen = static_cast< long long >( state.columns ) * static_cast< long long >( run );
		const long long position  = static_cast< long long >( column ) * static_cast< long long >( run )
			+ static_cast< long long >( drop ) * perScreen
			+ static_cast< long long >( row );

		cell.stream = WrapIndex( position, state.streamLength );
	}
	else
	{
		// Junk. Each cell draws on its own, and re-draws every 1/mutate seconds
		// -- but on its own schedule, offset by a per-cell phase. Without the
		// phase every glyph on screen would change on the same frame, which
		// reads as a cut rather than as churn.
		uint32_t cellSeed = HashCombine( columnSeed, static_cast< uint32_t >( row ) * 0x9E3779B9u + kSaltGlyph );

		if( state.mutate > 0.0f )
		{
			const float mutatePhase = Unit( HashCombine( cellSeed, kSaltMutate ) );
			const int tick = static_cast< int >( std::floor( state.time * state.mutate + mutatePhase ) );
			cellSeed = HashCombine( cellSeed, static_cast< uint32_t >( tick ) );
		}
		else
		{
			// Still glyphs, but still different on each pass of the drop --
			// otherwise a column repeats the same string forever and the eye
			// finds it immediately.
			cellSeed = HashCombine( cellSeed, static_cast< uint32_t >( drop ) );
		}

		cell.stream = state.streamLength > 0
			? static_cast< int >( Hash( cellSeed ) % static_cast< uint32_t >( state.streamLength ) )
			: 0;
	}

	return cell;
}

} // namespace downpour
