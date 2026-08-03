#include "Controls.h"

#include <algorithm>
#include <cmath>

namespace downpour
{
namespace
{
float Saturate( float v )
{
	return v < 0.0f ? 0.0f : ( v > 1.0f ? 1.0f : v );
}

/// Geometric interpolation: equal slider travel gives equal *ratio*, which is
/// how the eye reads a density or a rate.
float Exponential( float value, float low, float high )
{
	return low * std::pow( high / low, Saturate( value ) );
}

float Linear( float value, float low, float high )
{
	return low + ( high - low ) * Saturate( value );
}
} // namespace

int ColumnsFromParam( float value )
{
	return static_cast< int >( std::lround( Exponential( value, 8.0f, 240.0f ) ) );
}

int RowsForAspect( int columns, int width, int height )
{
	if( columns <= 0 || width <= 0 || height <= 0 )
		return 1;

	// Square cells. A cell's width is 1/columns of the frame, so the number of
	// rows that keeps it square is however many of that width fit down the
	// height.
	const float cellWidth = static_cast< float >( width ) / static_cast< float >( columns );
	const int rows        = static_cast< int >( std::lround( static_cast< float >( height ) / cellWidth ) );
	return std::max( 1, rows );
}

float SpeedFromParam( float value )
{
	return Exponential( value, 0.5f, 45.0f );
}

float TrailFromParam( float value )
{
	return Linear( value, 0.04f, 1.0f );
}

float MutateFromParam( float value )
{
	// Below a twentieth of the travel the answer is exactly zero. Without the
	// dead zone the bottom of an exponential range is 0.1 changes a second,
	// which looks like no mutation but is not, and the difference shows up as a
	// glyph flicking over once every ten seconds -- which reads as a bug.
	const float v = Saturate( value );
	if( v < 0.05f )
		return 0.0f;
	return Exponential( ( v - 0.05f ) / 0.95f, 0.5f, 30.0f );
}

float FalloffFromParam( float value )
{
	// Centred on 1: dragging away from the middle in either direction is a
	// visible change of the same size, which a linear 0.4..4 would not be.
	return Exponential( value, 0.4f, 4.0f );
}

uint32_t SeedFromParam( float value )
{
	// Quantised on purpose. The seed picks a rain, not a position in a
	// continuum, and a slider that changes the answer on every pixel of travel
	// cannot be set to the same thing twice.
	return static_cast< uint32_t >( 1 + static_cast< int >( std::lround( Saturate( value ) * 9998.0f ) ) );
}

float GlyphScaleFromParam( float value )
{
	const float v = Saturate( value );
	// Two straight segments meeting at 1.0 at three quarters of the travel.
	if( v <= 0.75f )
		return Linear( v / 0.75f, 0.25f, 1.0f );
	return Linear( ( v - 0.75f ) / 0.25f, 1.0f, 1.6f );
}

float HeadBoostFromParam( float value )
{
	return Saturate( value );
}

float GlowFromParam( float value )
{
	return Saturate( value ) * 2.0f;
}

} // namespace downpour
