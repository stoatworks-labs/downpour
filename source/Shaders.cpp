#include "Shaders.h"

namespace downpour
{

const char* const kVertexShader = R"(#version 410 core

layout( location = 0 ) in vec4 vPosition;
layout( location = 1 ) in vec2 vUV;

out vec2 uv;

void main()
{
	gl_Position = vPosition;
	uv = vUV;
}
)";

const char* const kEffectDefine    = "#define DOWNPOUR_OVER_INPUT 1\n";
const char* const kCellDebugDefine = "#define DOWNPOUR_DEBUG_CELLS 1\n";

const char* const kRainShader = R"(#version 410 core

in vec2 uv;
out vec4 fragColor;

//---------------------------------------------------------------------------
// The grid and the clock.
//---------------------------------------------------------------------------
uniform float Time;          //host clock, seconds
uniform vec2  Grid;          //columns, rows
uniform float Speed;         //rows per second, before per-column variation
uniform float Trail;         //trail length as a fraction of the run
uniform float Density;       //fraction of columns carrying a drop
uniform float Mutate;        //glyph changes per second
uniform float Falloff;       //brightness exponent along the trail
uniform float Audio[ 64 ];   //smoothed spectrum, low frequencies first
uniform float AudioLevel;    //0 ignores the spectrum entirely
uniform uint  Seed;
uniform int   DirectionMode; //0 down, 1 up, 2 right, 3 left
uniform int   FlowMode;      //0 scatter, 1 sequence

//---------------------------------------------------------------------------
// Glyphs.
//---------------------------------------------------------------------------
uniform sampler2D AtlasTexture;  //single channel, 255 for ink
uniform sampler2D StreamTexture; //atlas slot numbers -- texelFetch only

//vec2 and not ivec2, which is what it means. FFGLShader has no integer-vector
//setter, so an ivec2 could only be set by going around it with a raw
//glUniform2i -- and setting an ivec2 with glUniform2f is a GL_INVALID_OPERATION
//that leaves the uniform at zero, with no error anywhere the plugin can see.
//That cost an evening: StreamSize stayed (0,0), every glyph resolved to the
//blank slot, and the picture came out as faint mip-averaged smudges that looked
//like a sampling bug rather than an unset uniform.
uniform vec2 StreamSize;         //texels of StreamTexture actually in use
uniform int   StreamLength;      //entries in the stream
uniform vec2  AtlasLayout;       //slot columns, slot rows
uniform vec2  AtlasSize;         //the atlas texture's own size, in texels
uniform vec3  AtlasMetrics;      //slot size, glyph size, border, all in texels
uniform vec2  Resolution;        //output size in pixels
uniform float GlyphScale;        //glyph size within its cell, 1 = fills it
uniform float MirrorGlyphs;      //0 or 1

//---------------------------------------------------------------------------
// Colour.
//---------------------------------------------------------------------------
uniform vec3  TextColour;
uniform float TextOpacity;
uniform vec3  HeadColour;
uniform float HeadBoost;
uniform vec3  BackColour;
uniform float BackOpacity;
uniform float Glow;

#ifdef DOWNPOUR_OVER_INPUT
uniform sampler2D InputTexture;
uniform vec2 MaxUV;   //the part of the input texture that is really picture
uniform float Mix_;   //how much of the rain to lay over the clip
#endif

//===========================================================================
// The mirror of Rain.cpp starts here.
//===========================================================================

const float kSpeedMin = 0.45;      //= mirrored
const float kTrailMin = 0.35;      //= mirrored
const float kTailGap  = 0.6;       //= mirrored
const float kHeadRows = 1.5;       //= mirrored

const uint kSaltSpeed  = 0x9E3779B9u;//= mirrored
const uint kSaltPhase  = 0x85EBCA6Bu;//= mirrored
const uint kSaltTrail  = 0xC2B2AE35u;//= mirrored
const uint kSaltActive = 0x27D4EB2Fu;//= mirrored
const uint kSaltGlyph  = 0x165667B1u;//= mirrored
const uint kSaltMutate = 0xD3A2646Cu;//= mirrored

uint Hash( uint x )                                //= mirrored
{
	x ^= x >> 16;
	x *= 0x7FEB352Du;
	x ^= x >> 15;
	x *= 0x846CA68Bu;
	x ^= x >> 16;
	return x;
}

uint HashCombine( uint seed, uint v )              //= mirrored
{
	return Hash( seed ^ ( v * 0x9E3779B9u ) );
}

//The top 24 bits, which is the widest slice that converts to float without
//rounding. Using all 32 would round, and round differently on the CPU.
float Unit( uint h )                               //= mirrored
{
	return float( h >> 8 ) * ( 1.0 / 16777216.0 );
}

//===========================================================================
// Cell resolution.
//===========================================================================
struct CellResult
{
	float brightness;
	float head;
	int   stream;
};

CellResult EvaluateCell( ivec2 cellIndex )         //= mirrored
{
	CellResult result;
	result.brightness = 0.0;
	result.head       = 0.0;
	result.stream     = 0;

	int columns = int( Grid.x );
	int rows    = int( Grid.y );
	if( columns <= 0 || rows <= 0 )
		return result;

	//Direction, as a remap onto a thing that falls downwards.       //= mirrored
	int column;
	int row;
	if( DirectionMode == 1 )      { column = cellIndex.x; row = rows - 1 - cellIndex.y; }
	else if( DirectionMode == 2 ) { column = cellIndex.y; row = cellIndex.x; }
	else if( DirectionMode == 3 ) { column = cellIndex.y; row = columns - 1 - cellIndex.x; }
	else                          { column = cellIndex.x; row = cellIndex.y; }

	bool horizontal = ( DirectionMode == 2 || DirectionMode == 3 );
	float run = horizontal ? float( columns ) : float( rows );

	uint columnSeed = HashCombine( Seed, uint( column ) );           //= mirrored

	float speedScale = mix( kSpeedMin, 1.0, Unit( HashCombine( columnSeed, kSaltSpeed ) ) );
	float phase      = Unit( HashCombine( columnSeed, kSaltPhase ) );

	float cycle  = run * ( 1.0 + kTailGap );                         //= mirrored
	float travel = Time * Speed * speedScale + phase * cycle;        //= mirrored
	float drop   = floor( travel / cycle );                          //= mirrored
	float head   = travel - cycle * drop;                            //= mirrored

	//A column that loses the density draw carries no drop at all.   //= mirrored
	float activeDraw = Unit( HashCombine( columnSeed, kSaltActive ) );
	if( activeDraw >= Density )
		return result;

	float trailScale = mix( kTrailMin, 1.0, Unit( HashCombine( columnSeed, kSaltTrail ) ) );
	float trailRows  = max( 1.0, Trail * trailScale * run );         //= mirrored

	float distance = head - float( row );                            //= mirrored

	//Hard leading edge, soft tail -- which is what a phosphor does.  //= mirrored
	if( distance < 0.0 || distance > trailRows )
		return result;

	float along = distance / trailRows;
	result.brightness = pow( clamp( 1.0 - along, 0.0, 1.0 ), Falloff );
	result.head       = 1.0 - smoothstep( 0.0, kHeadRows, distance );

	//Audio: a per-column brightness gate from the host's FFT.        //= mirrored
	if( AudioLevel > 0.0 )
	{
		int columnCount = horizontal ? rows : columns;
		float across    = columnCount > 1 ? float( column ) / float( columnCount - 1 ) : 0.0;
		int bin         = min( 63, int( across * 64.0 ) );
		float gate      = mix( 1.0, Audio[ bin ], AudioLevel );
		result.brightness *= gate;
		result.head *= gate;
	}

	if( FlowMode == 1 )                                              //= mirrored
	{
		//Consecutive down a column, consecutive across columns, advancing by a
		//screenful per recycle, so a document plays out in order.
		//
		//Done in float because GLSL has no 64-bit integer here and a long
		//document times a large drop count overflows a 32-bit int in about
		//twenty minutes of running. A float32 carries 24 bits exactly, and the
		//modulo below is taken before anything larger can accumulate.
		float perScreen = float( columns ) * run;
		float position  = float( column ) * run + drop * perScreen + float( row );
		float wrapped   = position - float( StreamLength ) * floor( position / float( StreamLength ) );
		result.stream   = int( wrapped );
	}
	else
	{
		uint cellSeed = HashCombine( columnSeed, uint( row ) * 0x9E3779B9u + kSaltGlyph );

		if( Mutate > 0.0 )
		{
			float mutatePhase = Unit( HashCombine( cellSeed, kSaltMutate ) );
			int tick = int( floor( Time * Mutate + mutatePhase ) );
			cellSeed = HashCombine( cellSeed, uint( tick ) );
		}
		else
		{
			cellSeed = HashCombine( cellSeed, uint( int( drop ) ) );
		}

		result.stream = StreamLength > 0 ? int( Hash( cellSeed ) % uint( StreamLength ) ) : 0;
	}

	return result;
}

//===========================================================================
// End of the mirror.
//===========================================================================

///Atlas slot for a stream position. texelFetch, never texture: these are slot
///numbers, and an interpolated slot number addresses a glyph nobody chose.
int SlotForStream( int position )
{
	int stride = int( StreamSize.x );
	if( stride <= 0 )
		return 0;
	ivec2 texel = ivec2( position % stride, position / stride );
	return int( texelFetch( StreamTexture, texel, 0 ).r + 0.5 );
}

///Ink coverage at `local` (0..1 within the glyph body) for one atlas slot.
float SampleGlyph( int slot, vec2 local )
{
	if( any( lessThan( local, vec2( 0.0 ) ) ) || any( greaterThan( local, vec2( 1.0 ) ) ) )
		return 0.0;

	float slotPx   = AtlasMetrics.x;
	float glyphPx  = AtlasMetrics.y;
	float borderPx = AtlasMetrics.z;

	vec2 slotXY = vec2( float( slot % int( AtlasLayout.x ) ),
	                    float( slot / int( AtlasLayout.x ) ) );

	//**This is where the two vertical conventions meet, and it is the only
	//place either of them may change.**
	//
	//`local` comes from the cell grid, whose origin is top left and whose y runs
	//*down* the screen -- that is how falling rain is described and how Rain.cpp
	//is written. A GL texture's v runs *up*. Without this flip every glyph
	//renders upside down, which is nearly invisible on mirrored katakana, reads
	//as merely an odd typeface on lower case, and was caught only by
	//`--readback` scoring 34% against an atlas it should have matched exactly.
	vec2 upright = vec2( local.x, 1.0 - local.y );

	//Half a texel in on every side. GL_LINEAR at the very edge of the glyph
	//body takes half its weight from the border, and the border is blank -- so
	//without this every glyph loses its outermost row of ink to a fade.
	vec2 inset = vec2( 0.5 / glyphPx );
	vec2 body  = mix( inset, vec2( 1.0 ) - inset, upright );

	vec2 texel = slotXY * slotPx + vec2( borderPx ) + body * glyphPx;

	//The texture's own size, not the part of it the slots cover. 31 slots of 66
	//texels is 2046 of the 2048, and dividing by 2046 puts every glyph a tenth
	//of a percent off -- which is invisible in the middle of a stroke and shows
	//up as a sliver of the next slot along the far edge.
	return texture( AtlasTexture, texel / AtlasSize ).r;
}

void main()
{
	int columns = int( Grid.x );
	int rows    = int( Grid.y );

	//---------------------------------------------------------------------
	//Which cell, and where in it. uv has its origin bottom-left; the grid has
	//its origin top-left, to match Rain.cpp and to match the way anyone
	//describes falling rain. The flip happens here and nowhere else.
	//---------------------------------------------------------------------
	vec2 gridUV = vec2( uv.x, 1.0 - uv.y ) * Grid;
	ivec2 cellIndex = ivec2( floor( gridUV ) );
	cellIndex = clamp( cellIndex, ivec2( 0 ), ivec2( columns - 1, rows - 1 ) );

	vec2 within = gridUV - vec2( cellIndex );

	CellResult cell = EvaluateCell( cellIndex );

	//---------------------------------------------------------------------
	//Draw the glyph. The atlas is square, so the glyph is fitted into the
	//short axis of the cell and centred on the long one -- stretching it to
	//the cell's aspect is the difference between a font and a smeared font,
	//and cells are rarely square.
	//---------------------------------------------------------------------
	float ink = 0.0;
	if( cell.brightness > 0.0 )
	{
		//In **pixels**, not in frame units. A cell that is square on screen is
		//not square in 0..1 coordinates on anything but a square output, so
		//fitting the glyph in normalised space squashed every glyph by the
		//frame's aspect ratio -- 16:9 output, glyphs 56% of their proper height.
		vec2 cellPx = Resolution / Grid;
		float shortSide = min( cellPx.x, cellPx.y );
		vec2 span = vec2( shortSide ) / cellPx;        //square, as a fraction of the cell
		span *= max( GlyphScale, 0.001 );

		vec2 local = ( within - 0.5 * ( vec2( 1.0 ) - span ) ) / span;

		//The film's glyphs are mirrored. Doing it here rather than in the atlas
		//keeps GlyphData.cpp readable against a real font, which is what makes
		//a badly drawn glyph findable.
		local.x = mix( local.x, 1.0 - local.x, MirrorGlyphs );

		int slot = SlotForStream( cell.stream );
		ink = SampleGlyph( slot, local );
	}

	//---------------------------------------------------------------------
	//Composite.
	//---------------------------------------------------------------------
	vec3 lit = mix( TextColour, HeadColour, cell.head * HeadBoost );

	//Glow is a lift on the trail's own brightness rather than a blur: a blur
	//would need a second pass and a buffer, and at rain speed what reads as
	//glow is mostly the trail refusing to go fully dark.
	float body = cell.brightness + Glow * cell.brightness * ( 1.0 - cell.brightness );

	float textAlpha = clamp( ink * body * TextOpacity, 0.0, 1.0 );
	vec3  textRGB   = lit * textAlpha;   //premultiplied

#ifdef DOWNPOUR_DEBUG_CELLS
	//Raw cell state, for `dptest --rain` to compare against Rain.cpp. Written
	//to a float target so the comparison is exact rather than quantised to 8
	//bits, and compiled in **only** when the harness asks for it -- the shipping
	//shader does not carry this branch at all.
	fragColor = vec4( cell.brightness, cell.head, float( cell.stream ), 1.0 );
	return;
#endif

	vec4 back = vec4( BackColour * BackOpacity, BackOpacity );

#ifdef DOWNPOUR_OVER_INPUT
	//The clip sits under our own background, which sits under the rain. So
	//Background Opacity is how much the plugin veils the footage, and the two
	//controls keep the meanings they have in the source plugin.
	vec2 picture = clamp( uv, vec2( 0.0 ), vec2( 1.0 ) );
	vec4 clip = texture( InputTexture, picture * MaxUV );

	vec4 veiled = back + clip * ( 1.0 - back.a );
	vec4 rained = vec4( textRGB, textAlpha ) + veiled * ( 1.0 - textAlpha );

	fragColor = mix( clip, rained, clamp( Mix_, 0.0, 1.0 ) );
#else
	fragColor = vec4( textRGB, textAlpha ) + back * ( 1.0 - textAlpha );
#endif
}
)";

} // namespace downpour
