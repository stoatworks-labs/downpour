/**
    dptest -- the offline harness.

    It drives **the real plugin class** through the real FFGL sequence in a
    headless core-profile context. Not a reimplementation of the rain and not a
    preview: the thing under test is `DownpourPlugin`, compiled from the same
    objects that go into the bundles, and every number below comes out of a
    frame it actually rendered.

    Modes:

        --out PATH        render a frame
        --atlas PATH      dump the glyph atlas as built
        --list            parameters, with their types and defaults
      --presets         every factory preset survives every host behaviour
        --font            which codepoints came from where
        --rain            the GPU's cells against Rain.cpp, exactly
        --readback        render a document and read the text back off the frame

    `--rain` and `--readback` are the two that matter, and they check different
    halves. `--rain` compares the shader's arithmetic against the C++ mirror in
    `Rain.cpp` -- it would catch a constant changed on one side only. It cannot
    catch a glyph drawn in the wrong place, because it never looks at a glyph.
    `--readback` renders a known passage, identifies each character out of the
    pixels by correlating it against the atlas, and reassembles the string; that
    covers decode, atlas build, slot mapping, the stream texture and the glyph
    fetch, and it is the only test here that would notice the atlas being
    indexed off by one.

    Neither of them catches a dead uniform. See `tools/sweep.py`.
*/

#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>
#include <zlib.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "Corpus.h"
#include "Downpour.h"
#include "Glyphs.h"
#include "Rain.h"
#include "TextSource.h"
#include "Typeface.h"

using namespace downpour;

namespace
{
//---------------------------------------------------------------------------
// A PNG writer. zlib ships with the OS, so this is a few chunk headers and a
// CRC rather than a dependency.
//---------------------------------------------------------------------------
void putU32( std::vector< unsigned char >& out, uint32_t value )
{
	out.push_back( static_cast< unsigned char >( value >> 24 ) );
	out.push_back( static_cast< unsigned char >( value >> 16 ) );
	out.push_back( static_cast< unsigned char >( value >> 8 ) );
	out.push_back( static_cast< unsigned char >( value ) );
}

void putChunk( std::vector< unsigned char >& out, const char* type, const std::vector< unsigned char >& data )
{
	putU32( out, static_cast< uint32_t >( data.size() ) );
	const size_t start = out.size();
	out.insert( out.end(), type, type + 4 );
	out.insert( out.end(), data.begin(), data.end() );
	uLong crc = crc32( 0L, Z_NULL, 0 );
	crc       = crc32( crc, out.data() + start, static_cast< uInt >( 4 + data.size() ) );
	putU32( out, static_cast< uint32_t >( crc ) );
}

bool writePng( const std::string& path, int width, int height, const std::vector< unsigned char >& rgba )
{
	std::vector< unsigned char > raw;
	raw.reserve( static_cast< size_t >( height ) * ( 1 + static_cast< size_t >( width ) * 4 ) );
	for( int y = 0; y < height; ++y )
	{
		raw.push_back( 0 );// filter: none
		const unsigned char* row = rgba.data() + static_cast< size_t >( y ) * width * 4;
		raw.insert( raw.end(), row, row + static_cast< size_t >( width ) * 4 );
	}

	uLongf compressedSize = compressBound( static_cast< uLong >( raw.size() ) );
	std::vector< unsigned char > compressed( compressedSize );
	if( compress2( compressed.data(), &compressedSize, raw.data(), static_cast< uLong >( raw.size() ), 6 ) != Z_OK )
		return false;
	compressed.resize( compressedSize );

	std::vector< unsigned char > png = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };

	std::vector< unsigned char > ihdr;
	putU32( ihdr, static_cast< uint32_t >( width ) );
	putU32( ihdr, static_cast< uint32_t >( height ) );
	ihdr.push_back( 8 );// bit depth
	ihdr.push_back( 6 );// truecolour with alpha
	ihdr.push_back( 0 );
	ihdr.push_back( 0 );
	ihdr.push_back( 0 );
	putChunk( png, "IHDR", ihdr );
	putChunk( png, "IDAT", compressed );
	putChunk( png, "IEND", {} );

	FILE* file = fopen( path.c_str(), "wb" );
	if( file == nullptr )
		return false;
	const size_t written = fwrite( png.data(), 1, png.size(), file );
	fclose( file );
	return written == png.size();
}

//---------------------------------------------------------------------------
// GL plumbing.
//---------------------------------------------------------------------------
CGLContextObj createContext()
{
	const CGLPixelFormatAttribute accelerated[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFAAccelerated,
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};
	const CGLPixelFormatAttribute software[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};

	CGLPixelFormatObj format = nullptr;
	GLint formatCount        = 0;
	if( CGLChoosePixelFormat( accelerated, &format, &formatCount ) != kCGLNoError || format == nullptr )
	{
		if( CGLChoosePixelFormat( software, &format, &formatCount ) != kCGLNoError || format == nullptr )
			return nullptr;
	}

	CGLContextObj context = nullptr;
	const CGLError error  = CGLCreateContext( format, nullptr, &context );
	CGLDestroyPixelFormat( format );
	if( error != kCGLNoError )
		return nullptr;

	CGLSetCurrentContext( context );
	return context;
}

struct Target
{
	GLuint texture = 0;
	GLuint fbo     = 0;
	int width      = 0;
	int height     = 0;
	bool floating  = false;
};

Target makeTarget( int width, int height, bool floating )
{
	Target target;
	target.width    = width;
	target.height   = height;
	target.floating = floating;

	glGenTextures( 1, &target.texture );
	glBindTexture( GL_TEXTURE_2D, target.texture );
	glTexImage2D( GL_TEXTURE_2D, 0, floating ? GL_RGBA32F : GL_RGBA8, width, height, 0, GL_RGBA,
	              floating ? GL_FLOAT : GL_UNSIGNED_BYTE, nullptr );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
	glBindTexture( GL_TEXTURE_2D, 0 );

	glGenFramebuffers( 1, &target.fbo );
	glBindFramebuffer( GL_FRAMEBUFFER, target.fbo );
	glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, target.texture, 0 );
	return target;
}

void releaseTarget( Target& target )
{
	if( target.fbo != 0 )
		glDeleteFramebuffers( 1, &target.fbo );
	if( target.texture != 0 )
		glDeleteTextures( 1, &target.texture );
	target = Target();
}

/// Straight out of GL, bottom row first.
std::vector< unsigned char > readBytes( const Target& target )
{
	std::vector< unsigned char > pixels( static_cast< size_t >( target.width ) * target.height * 4 );
	glBindFramebuffer( GL_FRAMEBUFFER, target.fbo );
	glPixelStorei( GL_PACK_ALIGNMENT, 1 );
	glReadPixels( 0, 0, target.width, target.height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data() );
	return pixels;
}

std::vector< float > readFloats( const Target& target )
{
	std::vector< float > pixels( static_cast< size_t >( target.width ) * target.height * 4 );
	glBindFramebuffer( GL_FRAMEBUFFER, target.fbo );
	glPixelStorei( GL_PACK_ALIGNMENT, 1 );
	glReadPixels( 0, 0, target.width, target.height, GL_RGBA, GL_FLOAT, pixels.data() );
	return pixels;
}

std::vector< unsigned char > flipRows( const std::vector< unsigned char >& image, int width, int height )
{
	std::vector< unsigned char > flipped( image.size() );
	const size_t stride = static_cast< size_t >( width ) * 4;
	for( int y = 0; y < height; ++y )
		std::memcpy( flipped.data() + static_cast< size_t >( y ) * stride,
		             image.data() + static_cast< size_t >( height - 1 - y ) * stride, stride );
	return flipped;
}

//---------------------------------------------------------------------------
// Parameters by name.
//---------------------------------------------------------------------------
std::map< std::string, unsigned int > parameterIndex( DownpourPlugin& plugin )
{
	std::map< std::string, unsigned int > byName;
	for( unsigned int i = 0; i < plugin.GetNumParams(); ++i )
	{
		const char* name = plugin.GetParamName( i );
		if( name != nullptr )
			byName[ name ] = i;
	}
	return byName;
}

bool applySetting( DownpourPlugin& plugin, const std::string& assignment )
{
	const size_t equals = assignment.find( '=' );
	if( equals == std::string::npos )
	{
		std::fprintf( stderr, "--set wants Name=value, got '%s'\n", assignment.c_str() );
		return false;
	}

	const std::string name  = assignment.substr( 0, equals );
	const std::string value = assignment.substr( equals + 1 );

	const std::map< std::string, unsigned int > byName = parameterIndex( plugin );
	const auto found                                   = byName.find( name );
	if( found == byName.end() )
	{
		std::fprintf( stderr, "no parameter called '%s'\n", name.c_str() );
		return false;
	}

	if( plugin.GetParamType( found->second ) == FF_TYPE_TEXT
	    || plugin.GetParamType( found->second ) == FF_TYPE_FILE )
		plugin.SetTextParameter( found->second, value.c_str() );
	else
		plugin.SetFloatParameter( found->second, std::strtof( value.c_str(), nullptr ) );

	plugin.Invalidate();
	return true;
}

//---------------------------------------------------------------------------
// Driving the plugin.
//---------------------------------------------------------------------------
/// A test picture for the effect variant to draw over: coloured quadrants with a
/// gradient, so that Mix, Background Alpha and the premultiplied compositing
/// all have something to be visibly wrong against.
GLuint makeInputTexture( int width, int height )
{
	std::vector< unsigned char > pixels( static_cast< size_t >( width ) * height * 4 );
	for( int y = 0; y < height; ++y )
	{
		for( int x = 0; x < width; ++x )
		{
			const float u   = static_cast< float >( x ) / static_cast< float >( width );
			const float v   = static_cast< float >( y ) / static_cast< float >( height );
			unsigned char* p = pixels.data() + ( static_cast< size_t >( y ) * width + x ) * 4;
			p[ 0 ] = static_cast< unsigned char >( u * 255.0f );
			p[ 1 ] = static_cast< unsigned char >( v * 255.0f );
			p[ 2 ] = static_cast< unsigned char >( ( 1.0f - u ) * 200.0f );
			p[ 3 ] = 255;
		}
	}

	GLuint texture = 0;
	glGenTextures( 1, &texture );
	glBindTexture( GL_TEXTURE_2D, texture );
	glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data() );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	glBindTexture( GL_TEXTURE_2D, 0 );
	return texture;
}

bool render( DownpourPlugin& plugin, const Target& target, double time, GLuint input = 0 )
{
	FFGLViewportStruct viewport {};
	viewport.x      = 0;
	viewport.y      = 0;
	viewport.width  = static_cast< FFUInt32 >( target.width );
	viewport.height = static_cast< FFUInt32 >( target.height );

	plugin.SetClockScaleForTest( 1.0 );//seconds, said out loud rather than inferred
	plugin.SetTime( time );

	// A synthetic transport to go with the synthetic clock: 120 BPM in 4/4
	// from time zero, so bar N starts at exactly 2N seconds and the Beat and
	// Bar sync modes are as reproducible offline as Free is. Left at the
	// SDK's defaults, barPhase would sit frozen at zero and the synced rain
	// would step once a bar instead of kicking every beat.
	constexpr double kBarSeconds = 2.0;
	plugin.SetBeatInfo( 120.0f, static_cast< float >( std::fmod( time, kBarSeconds ) / kBarSeconds ) );

	// A synthetic spectrum too, written the way the host writes one: one value
	// per element of the Audio buffer. Without it Audio Level measurably does
	// nothing offline and the sweep would report it dead. A fixed shape rather
	// than anything time-driven, so renders stay reproducible: bass-heavy like
	// programme material, with a ripple so neighbouring columns differ.
	for( int bin = 0; bin < kAudioBins; ++bin )
	{
		const float across = static_cast< float >( bin ) / static_cast< float >( kAudioBins - 1 );
		const float level  = 0.7f * ( 1.0f - across ) * ( 1.0f - across ) + 0.2f * ( 0.5f + 0.5f * std::sin( 25.0f * across ) );
		plugin.SetParamElementValue( PT_AUDIO, static_cast< unsigned int >( bin ), level );
	}

	FFGLTextureStruct inputStruct {};
	inputStruct.Width = inputStruct.HardwareWidth = static_cast< FFUInt32 >( target.width );
	inputStruct.Height = inputStruct.HardwareHeight = static_cast< FFUInt32 >( target.height );
	inputStruct.Handle                              = input;
	FFGLTextureStruct* inputs[ 1 ]                  = { &inputStruct };

	ProcessOpenGLStruct process {};
	process.numInputTextures = input != 0 ? 1 : 0;
	process.inputTextures    = input != 0 ? inputs : nullptr;
	process.HostFBO          = target.fbo;

	glBindFramebuffer( GL_FRAMEBUFFER, target.fbo );
	glViewport( 0, 0, target.width, target.height );
	glClearColor( 0.0f, 0.0f, 0.0f, 0.0f );
	glClear( GL_COLOR_BUFFER_BIT );

	// The plugin reads the viewport out of its own base class, which is set by
	// InitGL. Re-setting it here is what lets one harness process render at
	// several sizes without tearing the GL resources down between them.
	plugin.InitGL( &viewport );
	return plugin.ProcessOpenGL( &process ) == FF_SUCCESS;
}

//---------------------------------------------------------------------------
// --atlas
//---------------------------------------------------------------------------
int dumpAtlas( DownpourPlugin& plugin, const std::string& path )
{
	const std::vector< uint8_t >& atlas = plugin.AtlasImage();
	if( atlas.size() < static_cast< size_t >( kAtlasPx ) * kAtlasPx )
	{
		std::fprintf( stderr, "no atlas has been built\n" );
		return 1;
	}

	std::vector< unsigned char > rgba( static_cast< size_t >( kAtlasPx ) * kAtlasPx * 4 );
	for( size_t i = 0; i < static_cast< size_t >( kAtlasPx ) * kAtlasPx; ++i )
	{
		const unsigned char ink = atlas[ i ];
		rgba[ i * 4 + 0 ]       = ink;
		rgba[ i * 4 + 1 ]       = ink;
		rgba[ i * 4 + 2 ]       = ink;
		rgba[ i * 4 + 3 ]       = 255;
	}

	// The atlas is stored bottom row first for GL; a PNG is top row first.
	const std::vector< unsigned char > flipped = flipRows( rgba, kAtlasPx, kAtlasPx );
	if( !writePng( path, kAtlasPx, kAtlasPx, flipped ) )
	{
		std::fprintf( stderr, "could not write %s\n", path.c_str() );
		return 1;
	}

	std::printf( "wrote %s (%d x %d, %d slots of %d px)\n", path.c_str(), kAtlasPx, kAtlasPx,
	             kAtlasSlots, kGlyphPx );
	return 0;
}

//---------------------------------------------------------------------------
// --font
//---------------------------------------------------------------------------
/// The built-in face's own invariants.
///
/// A glyph is drawn as twelve strings of twelve characters, and a typo in one of
/// them does not fail to compile -- it silently shortens a row, which reads as a
/// stroke that stops early. Nothing else in the repo would notice.
int faceInvariants()
{
	size_t count         = 0;
	const BitmapArt* art = FaceArt( count );

	int bad = 0;
	std::map< uint32_t, int > seen;

	for( size_t i = 0; i < count; ++i )
	{
		for( int row = 0; row < kBitmapSize; ++row )
		{
			const char* line = art[ i ].rows[ row ];
			if( line == nullptr )
			{
				std::printf( "  FAIL glyph U+%04X row %d is null\n", art[ i ].codepoint, row );
				++bad;
				continue;
			}
			const size_t length = std::strlen( line );
			if( length != static_cast< size_t >( kBitmapSize ) )
			{
				std::printf( "  FAIL glyph U+%04X row %d is %zu characters, expected %d\n",
				             art[ i ].codepoint, row, length, kBitmapSize );
				++bad;
			}
			for( size_t c = 0; c < length; ++c )
			{
				if( line[ c ] != '#' && line[ c ] != '.' )
				{
					std::printf( "  FAIL glyph U+%04X row %d has '%c', expected '#' or '.'\n",
					             art[ i ].codepoint, row, line[ c ] );
					++bad;
					break;
				}
			}
		}

		if( ++seen[ art[ i ].codepoint ] == 2 )
		{
			std::printf( "  FAIL codepoint U+%04X is drawn twice; the second is unreachable\n",
			             art[ i ].codepoint );
			++bad;
		}
	}

	if( bad == 0 )
		std::printf( "  ok   %zu glyphs, every row %d cells of '#' or '.', no duplicates\n",
		             count, kBitmapSize );
	return bad == 0 ? 0 : 1;
}

int fontReport( DownpourPlugin& plugin )
{
	std::printf( "built-in face: %d glyphs on a %dx%d body\n", BuiltinCount(), kBitmapSize, kBitmapSize );

	const std::vector< FontFile >& fonts = InstalledFonts();
	std::printf( "installed families: %zu\n", fonts.size() );
	for( size_t i = 0; i < fonts.size() && i < 8; ++i )
		std::printf( "    %s\n", fonts[ i ].family.c_str() );
	if( fonts.size() > 8 )
		std::printf( "    ... and %zu more\n", fonts.size() - 8 );

	std::printf( "current source: %s\n", plugin.SourceNote().c_str() );

	const int invariants = faceInvariants();

	int drawn   = 0;
	int missing = 0;
	std::string missingList;
	std::vector< uint32_t > seen;

	for( uint32_t codepoint : plugin.StreamCodepoints() )
	{
		if( std::find( seen.begin(), seen.end(), codepoint ) != seen.end() )
			continue;
		seen.push_back( codepoint );

		if( plugin.SlotForCodepoint( codepoint ) > kBlankSlot )
		{
			++drawn;
		}
		else if( codepoint != ' ' )
		{
			++missing;
			if( missingList.size() < 200 )
				missingList += EncodeUtf8( codepoint ) + " ";
		}
	}

	const DownpourPlugin::AtlasCounts counts = plugin.LastAtlasCounts();
	std::printf( "stream uses %zu distinct codepoints: %d drawn, %d undrawable\n", seen.size(), drawn, missing );
	std::printf( "atlas: %d glyphs from the chosen font, %d from the built-in face, %d with neither\n",
	             counts.fromFont, counts.fromBuiltin, counts.missing );
	if( missing > 0 )
		std::printf( "  undrawable: %s\n", missingList.c_str() );

	// A codepoint nothing can draw is not automatically a failure -- it is a
	// failure only if it is silent, which is what this line exists to prevent.
	// The face's own invariants are, though.
	return invariants;
}

//---------------------------------------------------------------------------
// --list
//---------------------------------------------------------------------------
int listParameters( DownpourPlugin& plugin )
{
	static const std::map< unsigned int, const char* > kTypeNames = {
		{ FF_TYPE_STANDARD, "standard" }, { FF_TYPE_BOOLEAN, "boolean" },
		{ FF_TYPE_OPTION, "option" },     { FF_TYPE_TEXT, "text" },
		{ FF_TYPE_FILE, "file" },         { FF_TYPE_RED, "red" },
		{ FF_TYPE_GREEN, "green" },       { FF_TYPE_BLUE, "blue" },
		{ FF_TYPE_BUFFER, "buffer" },
	};

	std::printf( "%-4s %-22s %-10s %s\n", "id", "name", "type", "value" );
	for( unsigned int i = 0; i < plugin.GetNumParams(); ++i )
	{
		const unsigned int type = plugin.GetParamType( i );
		const auto typeName     = kTypeNames.find( type );

		std::string value;
		if( type == FF_TYPE_TEXT || type == FF_TYPE_FILE )
		{
			const char* text = plugin.GetTextParameter( i );
			value            = text != nullptr ? std::string( "\"" ) + text + "\"" : "\"\"";
		}
		else
		{
			char scratch[ 32 ];
			std::snprintf( scratch, sizeof( scratch ), "%.4f", plugin.GetFloatParameter( i ) );
			value = scratch;
		}

		std::printf( "%-4u %-22s %-10s %s\n", i, plugin.GetParamName( i ),
		             typeName == kTypeNames.end() ? "?" : typeName->second, value.c_str() );
	}
	return 0;
}

//---------------------------------------------------------------------------
// --rain
//
// The shader's own cell state against Rain.cpp, exactly. Rendered to a float
// target through the debug shader so nothing is quantised on the way out.
//---------------------------------------------------------------------------
int rainCheck()
{
	struct Case
	{
		const char* label;
		int width;
		int height;
		double time;
		float columns;
		float direction;
		float speed;
		float trail;
		float density;
		float mutate;
		float audioLevel;
	};

	// Deliberately awkward: a wrapped drop, an odd aspect, every direction, and
	// mutation both on and off, because the mutate branch and the drop branch
	// are the two places the two implementations could disagree.
	const Case cases[] = {
		{ "down, 1080p",       1920, 1080,  3.25, 0.59f, 0.0f, 0.52f, 0.43f, 0.85f, 0.55f, 0.0f },
		{ "down, 720p",        1280,  720,  3.25, 0.59f, 0.0f, 0.52f, 0.43f, 0.85f, 0.55f, 0.0f },
		{ "down, 4K",          3840, 2160,  3.25, 0.59f, 0.0f, 0.52f, 0.43f, 0.85f, 0.55f, 0.0f },
		{ "up",                1920, 1080, 61.75, 0.42f, 1.0f, 0.70f, 0.60f, 1.00f, 0.00f, 0.0f },
		{ "right",             1600,  900, 12.50, 0.35f, 2.0f, 0.40f, 0.30f, 0.70f, 0.80f, 0.0f },
		{ "left",               900, 1600, 91.70, 0.80f, 3.0f, 0.65f, 0.90f, 0.50f, 0.20f, 0.0f },
		{ "still glyphs",      1920, 1080, 44.10, 0.59f, 0.0f, 0.30f, 0.50f, 1.00f, 0.00f, 0.0f },
		{ "dense, long trail", 1280,  720, 155.9, 0.70f, 0.0f, 0.85f, 1.00f, 1.00f, 0.95f, 0.0f },
		//The audio gate is mirrored arithmetic like everything above, so it
		//gets a case: full Audio Level over the synthetic spectrum, on a
		//horizontal run so the column-count remap is exercised too.
		{ "audio gate",        1920, 1080,  3.25, 0.59f, 0.0f, 0.52f, 0.43f, 0.85f, 0.55f, 1.0f },
		{ "audio gate, right", 1600,  900, 12.50, 0.35f, 2.0f, 0.40f, 0.30f, 0.70f, 0.80f, 1.0f },
	};

	int failures = 0;
	int compared = 0;
	int exempt   = 0;

	for( const Case& c : cases )
	{
		DownpourPlugin plugin( false );
		plugin.EnableCellDebug();

		plugin.SetFloatParameter( PT_COLUMNS, c.columns );
		plugin.SetFloatParameter( PT_DIRECTION, c.direction );
		plugin.SetFloatParameter( PT_SPEED, c.speed );
		plugin.SetFloatParameter( PT_TRAIL, c.trail );
		plugin.SetFloatParameter( PT_DENSITY, c.density );
		plugin.SetFloatParameter( PT_MUTATE, c.mutate );
		plugin.SetFloatParameter( PT_AUDIO_LEVEL, c.audioLevel );

		Target target = makeTarget( c.width, c.height, true );
		if( !render( plugin, target, c.time ) )
		{
			std::fprintf( stderr, "%s: render failed\n", c.label );
			releaseTarget( target );
			++failures;
			continue;
		}

		const std::vector< float > pixels = readFloats( target );
		const RainState state             = plugin.CurrentState( c.width, c.height );

		int mismatches      = 0;
		float worstBright   = 0.0f;
		float worstHead     = 0.0f;
		int firstBadColumn  = -1;
		int firstBadRow     = -1;

		//-----------------------------------------------------------------
		// The tolerance is not a constant, because the precision available is
		// not a constant.
		//
		// The head's position is `travel - cycle * drop`: two large, nearly
		// equal numbers subtracted to leave a small one. `travel` grows without
		// bound with the host clock -- at two and a half minutes and twenty-odd
		// rows a second it is already about 4700 -- and float32 carries 24 bits,
		// so the *absolute* resolution of the remainder degrades in proportion.
		// At t=156 that is around 3e-4 of a row, and smoothstep passes it
		// straight through into `head`.
		//
		// This is a property of the plugin, not of the test, and it is not worth
		// engineering away: a ten-hour show reaches an error of about a
		// hundredth of a cell, which is a hundredth of a character's width. What
		// would be wrong is a flat tolerance loose enough to cover the worst
		// case, because it would also cover a genuine divergence in every short
		// case. So it scales with the thing that causes it.
		const float travelScale = state.travel;
		const float tolerance   = 2e-4f + travelScale * 1.5e-7f;

		for( int row = 0; row < state.rows; ++row )
		{
			for( int column = 0; column < state.columns; ++column )
			{
				// Sample the centre of the cell. The grid has its origin top
				// left and GL's framebuffer has it bottom left, which is the
				// one flip in this comparison.
				const int px = static_cast< int >( ( column + 0.5f ) * c.width / state.columns );
				const int py = c.height - 1 - static_cast< int >( ( row + 0.5f ) * c.height / state.rows );
				if( px < 0 || py < 0 || px >= c.width || py >= c.height )
					continue;

				const float* texel = pixels.data() + ( static_cast< size_t >( py ) * c.width + px ) * 4;
				const Cell expected = Evaluate( state, column, row );

				const float brightDelta = std::fabs( texel[ 0 ] - expected.brightness );
				const float headDelta   = std::fabs( texel[ 1 ] - expected.head );
				const int gpuStream     = static_cast< int >( texel[ 2 ] + 0.5f );

				worstBright = std::max( worstBright, brightDelta );
				worstHead   = std::max( worstHead, headDelta );

				// A tolerance, not equality. `pow` and `smoothstep` are allowed
				// to differ in the last bit or two between a CPU's libm and a
				// GPU's; the hash and the stream index are integers and are
				// compared exactly, which is where a real divergence would show.
				// A cell whose mutation tick is about to roll over is exempt
				// from the *glyph* comparison, though not from the brightness
				// one.
				//
				// The tick is `floor( time * mutate + phase )`, and after two
				// and a half minutes at twenty-four changes a second that
				// product is a four-digit float32. The GPU is entitled to fold
				// the multiply and add into an FMA and the CPU is not, so the
				// two land on opposite sides of an integer boundary perhaps
				// twice in four thousand cells -- and a tick that differs by one
				// hashes to an unrelated glyph, which looks like a catastrophic
				// disagreement rather than the last-bit one it is.
				//
				// It is not worth engineering away. At the instant it happens
				// the glyph is being replaced anyway, so the two answers are
				// both "correct" for adjacent frames. What would not be
				// acceptable is the test quietly widening its tolerance, so the
				// exemption is narrow, explicit, and counted.
				bool onMutationBoundary = false;
				if( state.flow == Flow::Scatter && state.mutate > 0.0f && expected.brightness > 0.0f )
				{
					const uint32_t cellSeed = HashCombine(
						HashCombine( state.seed, static_cast< uint32_t >( column ) ),
						static_cast< uint32_t >( row ) * 0x9E3779B9u + 0x165667B1u );
					const float phase = Unit( HashCombine( cellSeed, 0xD3A2646Cu ) );
					const float exact = state.mutateTicks + phase;
					const float frac  = exact - std::floor( exact );
					onMutationBoundary = frac < 1e-3f || frac > 1.0f - 1e-3f;
				}

				if( onMutationBoundary )
					++exempt;

				const bool bad = brightDelta > tolerance || headDelta > tolerance
					|| ( expected.brightness > 0.0f && !onMutationBoundary && gpuStream != expected.stream );

				if( bad )
				{
					if( mismatches == 0 )
					{
						firstBadColumn = column;
						firstBadRow    = row;
						std::printf( "       gpu b=%.7f h=%.7f s=%d | cpu b=%.7f h=%.7f s=%d\n",
						             texel[ 0 ], texel[ 1 ], gpuStream,
						             expected.brightness, expected.head, expected.stream );
					}
					++mismatches;
				}
				++compared;
			}
		}

		releaseTarget( target );

		if( mismatches > 0 )
		{
			std::printf( "  FAIL %-18s %d mismatches, first at column %d row %d\n",
			             c.label, mismatches, firstBadColumn, firstBadRow );
			++failures;
		}
		else
		{
			std::printf( "  ok   %-18s %d x %d cells, worst brightness %.2e, worst head %.2e (tol %.2e)\n",
			             c.label, state.columns, state.rows, worstBright, worstHead, tolerance );
		}
	}

	std::printf( "%d cells compared against Rain.cpp, %d exempt on a mutation boundary, %d cases failed\n",
	             compared, exempt, failures );

	// If the exemption ever starts covering a real fraction of the frame it has
	// stopped being a rounding allowance and started hiding something.
	if( compared > 0 && exempt * 100 > compared )
	{
		std::printf( "  FAIL %d%% of cells were exempt, which is too many to be float noise\n",
		             exempt * 100 / compared );
		++failures;
	}

	return failures == 0 ? 0 : 1;
}

//---------------------------------------------------------------------------
// --readback
//
// Render a known passage and read the characters back out of the pixels by
// correlating each cell against the atlas. Covers everything --rain does not:
// the UTF-8 decode, the atlas build, the codepoint-to-slot map, the stream
// texture and the glyph fetch.
//---------------------------------------------------------------------------
int readbackCheck()
{
	// One output pixel per atlas texel, so a cell in the frame is the glyph at
	// its native size and the correlation is not fighting a resample.
	const int columns = 24;
	const int rows    = 14;
	const int width   = columns * kGlyphPx;
	const int height  = rows * kGlyphPx;

	DownpourPlugin plugin( false );

	// Alice, still glyphs, no mirroring, glyph filling the cell, and colours
	// that make ink unambiguous. Mutation off matters: a mutating glyph is
	// still correct, but it is chosen from a tick derived from the clock, and
	// the point here is to read the *document* back rather than the hash.
	plugin.SetFloatParameter( PT_SOURCE, static_cast< float >( Source::Alice ) );
	plugin.SetFloatParameter( PT_MUTATE, 0.0f );
	plugin.SetFloatParameter( PT_MIRROR, 0.0f );
	plugin.SetFloatParameter( PT_GLYPH_SCALE, 0.75f );// = 1.0
	plugin.SetFloatParameter( PT_COLUMNS, 0.0f );     // overridden below
	plugin.SetFloatParameter( PT_DENSITY, 1.0f );
	plugin.SetFloatParameter( PT_TRAIL, 1.0f );
	plugin.SetFloatParameter( PT_FALLOFF, 0.5f );
	plugin.SetFloatParameter( PT_GLOW, 0.0f );
	plugin.SetFloatParameter( PT_HEAD_BOOST, 0.0f );
	plugin.SetFloatParameter( PT_TEXT_R, 1.0f );
	plugin.SetFloatParameter( PT_TEXT_G, 1.0f );
	plugin.SetFloatParameter( PT_TEXT_B, 1.0f );
	plugin.SetFloatParameter( PT_TEXT_OPACITY, 1.0f );
	plugin.SetFloatParameter( PT_BACK_OPACITY, 1.0f );
	plugin.SetFloatParameter( PT_BACK_R, 0.0f );
	plugin.SetFloatParameter( PT_BACK_G, 0.0f );
	plugin.SetFloatParameter( PT_BACK_B, 0.0f );

	// Find the parameter value that gives exactly `columns` columns rather than
	// assuming the curve -- the curve is Controls.cpp's business and this test
	// should not encode a second copy of it.
	float columnsParam = 0.0f;
	for( int step = 0; step <= 1000; ++step )
	{
		const float candidate = static_cast< float >( step ) / 1000.0f;
		if( ColumnsFromParam( candidate ) >= columns )
		{
			columnsParam = candidate;
			break;
		}
	}
	plugin.SetFloatParameter( PT_COLUMNS, columnsParam );
	plugin.Invalidate();

	Target target = makeTarget( width, height, false );
	if( !render( plugin, target, 7.0 ) )
	{
		std::fprintf( stderr, "readback: render failed\n" );
		releaseTarget( target );
		return 1;
	}

	const std::vector< unsigned char > pixels = readBytes( target );
	const RainState state                     = plugin.CurrentState( width, height );

	if( state.columns != columns )
		std::printf( "  note: asked for %d columns, got %d\n", columns, state.columns );

	// Invert the slot map so a recognised glyph can be named.
	std::map< int, uint32_t > codepointForSlot;
	for( uint32_t codepoint : plugin.StreamCodepoints() )
	{
		const int slot = plugin.SlotForCodepoint( codepoint );
		if( slot > kBlankSlot )
			codepointForSlot[ slot ] = codepoint;
	}

	const std::vector< uint8_t >& atlas = plugin.AtlasImage();

	int read       = 0;
	int correct    = 0;
	int unreadable = 0;
	int litCells   = 0;
	int spaceCells = 0;
	std::string got;
	std::string want;

	for( int row = 0; row < state.rows; ++row )
	{
		for( int column = 0; column < state.columns; ++column )
		{
			const Cell cell = Evaluate( state, column, row );
			if( cell.brightness < 0.35f )
				continue;
			++litCells;

			const uint32_t expected = plugin.StreamCodepoints()[ static_cast< size_t >( cell.stream ) ];
			if( expected == ' ' )
			{
				++spaceCells;
				continue;
			}

			//-------------------------------------------------------------
			// Pull the cell out of the frame, normalised by its own peak so
			// the trail's brightness falls out of the comparison.
			//-------------------------------------------------------------
			const int originX = column * kGlyphPx;
			const int originY = height - ( row + 1 ) * kGlyphPx;

			std::vector< float > block( static_cast< size_t >( kGlyphPx ) * kGlyphPx, 0.0f );
			float peak = 0.0f;
			for( int y = 0; y < kGlyphPx; ++y )
			{
				for( int x = 0; x < kGlyphPx; ++x )
				{
					const int px = originX + x;
					const int py = originY + y;
					if( px < 0 || py < 0 || px >= width || py >= height )
						continue;
					const float value = pixels[ ( static_cast< size_t >( py ) * width + px ) * 4 ] / 255.0f;
					block[ static_cast< size_t >( y ) * kGlyphPx + x ] = value;
					peak = std::max( peak, value );
				}
			}

			if( peak < 0.08f )
			{
				++unreadable;
				continue;
			}
			for( float& value : block )
				value /= peak;

			//-------------------------------------------------------------
			// Correlate against every slot the stream can produce.
			//-------------------------------------------------------------
			int bestSlot     = -1;
			double bestScore = -1.0;
			for( const auto& entry : codepointForSlot )
			{
				int atlasX = 0, atlasY = 0;
				SlotOrigin( entry.first, atlasX, atlasY );

				double dot = 0.0, normA = 0.0, normB = 0.0;
				for( int y = 0; y < kGlyphPx; ++y )
				{
					for( int x = 0; x < kGlyphPx; ++x )
					{
						const double a = block[ static_cast< size_t >( y ) * kGlyphPx + x ];
						const double b = atlas[ static_cast< size_t >( atlasY + y ) * kAtlasPx + atlasX + x ] / 255.0;
						dot += a * b;
						normA += a * a;
						normB += b * b;
					}
				}

				if( normA <= 0.0 || normB <= 0.0 )
					continue;
				const double score = dot / std::sqrt( normA * normB );
				if( score > bestScore )
				{
					bestScore = score;
					bestSlot  = entry.first;
				}
			}

			if( bestSlot < 0 )
			{
				++unreadable;
				continue;
			}

			++read;
			const uint32_t decoded = codepointForSlot[ bestSlot ];
			if( decoded == expected )
				++correct;
			else if( read - correct <= 4 )
				std::printf( "    mismatch col %d row %d: stream %d -> expect '%s' (slot %d), "
				             "read '%s' (slot %d, score %.4f)\n",
				             column, row, cell.stream, EncodeUtf8( expected ).c_str(),
				             plugin.SlotForCodepoint( expected ), EncodeUtf8( decoded ).c_str(),
				             bestSlot, bestScore );

			if( got.size() < 60 )
			{
				got += EncodeUtf8( decoded );
				want += EncodeUtf8( expected );
			}
		}
	}

	releaseTarget( target );

	if( read == 0 )
	{
		std::fprintf( stderr,
		              "readback: nothing legible was rendered\n"
		              "  grid %dx%d, frame %dx%d, stream %zu codepoints (%s), %d slots mapped,\n"
		              "  %d cells bright enough, %d skipped as spaces, %d too dim to correlate\n",
		              state.columns, state.rows, width, height, plugin.StreamCodepoints().size(),
		              plugin.SourceNote().c_str(), static_cast< int >( codepointForSlot.size() ),
		              litCells, spaceCells, unreadable );
		return 1;
	}

	const double rate = 100.0 * correct / read;
	std::printf( "  read %d characters off the frame, %d correct (%.2f%%), %d too dim\n",
	             read, correct, rate, unreadable );
	std::printf( "  expected: %s\n", want.c_str() );
	std::printf( "  got:      %s\n", got.c_str() );

	// Every character or it is a failure. Correlation against a known atlas at
	// native size is not a fuzzy problem: an occasional wrong answer would mean
	// two glyphs really are identical in the atlas, and that is worth knowing
	// rather than tolerating.
	if( correct != read )
	{
		std::printf( "  FAIL %d characters came back wrong\n", read - correct );
		return 1;
	}

	std::printf( "  ok   the document round-trips through the atlas and the shader\n" );
	return 0;
}
//---------------------------------------------------------------------------
// --sequence
//
// Renders a run of frames in one process, driving the plugin's own parameters
// from a cue sheet. That is the point: the project video is the real plugin
// being operated, not a mock-up or a screen recording of something else.
//
// One process rather than one per frame because the font scan, the atlas build
// and the GL context all happen once. A thousand frames through the shell would
// be twenty minutes of process startup.
//
// Cue syntax, one per line, `#` for comments:
//
//     12.0        Speed=0.8            set at t=12s
//     4.0..9.0    Columns=0.3..0.7     ramp between two times
//
// Times are seconds on the video's own clock, which is also the host clock
// handed to the plugin -- so a cue at 12s is the frame you see at 12s.
//---------------------------------------------------------------------------
struct Cue
{
	double from  = 0.0;
	double to    = 0.0;
	std::string name;
	float first  = 0.0f;
	float second = 0.0f;
	bool ramp    = false;
	bool text    = false;
	std::string textValue;
};

bool parseCues( const std::string& path, std::vector< Cue >& cues )
{
	FILE* file = fopen( path.c_str(), "rb" );
	if( file == nullptr )
	{
		std::fprintf( stderr, "cannot open cue sheet %s\n", path.c_str() );
		return false;
	}

	char line[ 1024 ];
	int number = 0;
	while( fgets( line, sizeof( line ), file ) != nullptr )
	{
		++number;
		std::string text = line;

		const size_t hash = text.find( '#' );
		if( hash != std::string::npos )
			text = text.substr( 0, hash );

		const size_t firstReal = text.find_first_not_of( " \t\r\n" );
		if( firstReal == std::string::npos )
			continue;
		text = text.substr( firstReal );

		const size_t split = text.find_first_of( " \t" );
		if( split == std::string::npos )
			continue;

		const std::string when       = text.substr( 0, split );
		std::string assignment       = text.substr( split );
		const size_t assignStart     = assignment.find_first_not_of( " \t" );
		if( assignStart == std::string::npos )
			continue;
		assignment = assignment.substr( assignStart );
		while( !assignment.empty() && ( assignment.back() == '\n' || assignment.back() == '\r'
		                                || assignment.back() == ' ' || assignment.back() == '\t' ) )
			assignment.pop_back();

		Cue cue;
		const size_t timeRange = when.find( ".." );
		if( timeRange != std::string::npos )
		{
			cue.from = std::strtod( when.substr( 0, timeRange ).c_str(), nullptr );
			cue.to   = std::strtod( when.substr( timeRange + 2 ).c_str(), nullptr );
			cue.ramp = true;
		}
		else
		{
			cue.from = cue.to = std::strtod( when.c_str(), nullptr );
		}

		const size_t equals = assignment.find( '=' );
		if( equals == std::string::npos )
		{
			std::fprintf( stderr, "%s:%d: expected Name=value\n", path.c_str(), number );
			return false;
		}

		cue.name           = assignment.substr( 0, equals );
		const std::string value = assignment.substr( equals + 1 );

		const size_t valueRange = value.find( ".." );
		if( cue.ramp && valueRange != std::string::npos )
		{
			cue.first  = std::strtof( value.substr( 0, valueRange ).c_str(), nullptr );
			cue.second = std::strtof( value.substr( valueRange + 2 ).c_str(), nullptr );
		}
		else if( value.find_first_not_of( "0123456789.-+eE" ) != std::string::npos )
		{
			cue.text      = true;
			cue.textValue = value;
			cue.ramp      = false;
		}
		else
		{
			cue.first = cue.second = std::strtof( value.c_str(), nullptr );
			cue.ramp  = false;
		}

		cues.push_back( cue );
	}

	fclose( file );
	return true;
}

int renderSequence( const std::string& directory,
                    const std::string& cuePath,
                    int width,
                    int height,
                    double seconds,
                    double fps,
                    bool effect )
{
	std::vector< Cue > cues;
	if( !cuePath.empty() && !parseCues( cuePath, cues ) )
		return 1;

	DownpourPlugin plugin( effect );
	const std::map< std::string, unsigned int > byName = parameterIndex( plugin );

	for( const Cue& cue : cues )
	{
		if( byName.find( cue.name ) == byName.end() )
		{
			std::fprintf( stderr, "cue names '%s', which is not a parameter\n", cue.name.c_str() );
			return 1;
		}
	}

	Target target      = makeTarget( width, height, false );
	const GLuint input = effect ? makeInputTexture( width, height ) : 0;

	const int frames = static_cast< int >( seconds * fps + 0.5 );
	int written      = 0;

	for( int frame = 0; frame < frames; ++frame )
	{
		const double now = static_cast< double >( frame ) / fps;

		// Apply every cue whose window has started. Cues are applied in file
		// order each frame rather than tracked as state, so a later cue on the
		// same parameter simply wins -- which is what reading the sheet top to
		// bottom would lead you to expect.
		for( const Cue& cue : cues )
		{
			if( now < cue.from )
				continue;

			const unsigned int index = byName.at( cue.name );

			if( cue.text )
			{
				plugin.SetTextParameter( index, cue.textValue.c_str() );
				plugin.Invalidate();
				continue;
			}

			float value = cue.second;
			if( cue.ramp && now < cue.to && cue.to > cue.from )
			{
				const double t = ( now - cue.from ) / ( cue.to - cue.from );
				// Smoothstep rather than linear. A parameter that starts and
				// stops abruptly reads as a jump cut even when the value in
				// between is right.
				const double eased = t * t * ( 3.0 - 2.0 * t );
				value = static_cast< float >( cue.first + ( cue.second - cue.first ) * eased );
			}

			plugin.SetFloatParameter( index, value );
			if( index == PT_SOURCE || index == PT_CHARSET || index == PT_FONT )
				plugin.Invalidate();
		}

		if( !render( plugin, target, now, input ) )
		{
			std::fprintf( stderr, "frame %d failed\n", frame );
			releaseTarget( target );
			return 1;
		}

		char path[ 1024 ];
		std::snprintf( path, sizeof( path ), "%s/frame%05d.png", directory.c_str(), frame );

		const std::vector< unsigned char > pixels = readBytes( target );
		const std::vector< unsigned char > image  = flipRows( pixels, width, height );
		if( !writePng( path, width, height, image ) )
		{
			std::fprintf( stderr, "could not write %s\n", path );
			releaseTarget( target );
			return 1;
		}

		++written;
		if( written % 60 == 0 )
			std::printf( "  %d / %d frames\n", written, frames );
	}

	releaseTarget( target );
	if( input != 0 )
		glDeleteTextures( 1, &input );
	plugin.DeInitGL();

	std::printf( "wrote %d frames to %s at %g fps (%.1f seconds)\n", written, directory.c_str(), fps,
	             written / fps );
	return 0;
}
} // namespace

//---------------------------------------------------------------------------
/// Prove a factory preset survives whatever the host does next.
///
/// FFGL's host owns parameter state and is free to push it back down at any
/// time, and nothing in the specification obliges it to act on the value
/// events a plugin raises when it changes a parameter itself. So there are
/// three hosts to survive, and the plugin cannot tell which one it is talking
/// to:
///
///   - one that honours the events and hands the new values straight back;
///   - one that ignores them and carries on restating the values it still
///     believes in, which are the ones from before the preset;
///   - one that honours them but keeps its parameters shorter than a float, so
///     what comes back is near the preset rather than equal to it.
///
/// All three arrive as SetFloatParameter calls carrying a changed value, which
/// is why "the value changed, so the operator must have taken over" is the
/// wrong test. Resolume is the second kind, and against the unfixed code this
/// fails in exactly that column -- reported as vertigo issue #2.
///
/// No GL here: this is the parameter plumbing, not the picture.
//---------------------------------------------------------------------------
int runPresetTest()
{
	using namespace downpour::presets;

	int coveredCount            = 0;
	const unsigned int* covered = DownpourPlugin::PresetParamIDsForTest( coveredCount );

	enum class Host
	{
		Honours,
		Ignores,
		Quantises
	};
	struct HostCase
	{
		Host kind;
		const char* name;
	};
	const HostCase hosts[] = {
		{ Host::Honours, "honours value events" },
		{ Host::Ignores, "ignores value events" },
		{ Host::Quantises, "honours, 1/1000 steps" },
	};

	int failures = 0;

	for( const HostCase& host : hosts )
	{
		for( int preset = 1; preset <= kCount; ++preset )
		{
			// The source build; the effect declares the same parameters.
			DownpourPlugin plugin( false );

			int presetIndex = -1;
			for( unsigned int i = 0; i < plugin.GetNumParams(); ++i )
			{
				const char* declared = plugin.GetParamName( i );
				if( declared != nullptr && std::strcmp( declared, "Preset" ) == 0 )
				{
					presetIndex = int( i );
					break;
				}
			}
			if( presetIndex < 0 )
			{
				std::fprintf( stderr, "presets: no parameter is called \"Preset\"\n" );
				return 1;
			}

			// What the host thinks the sliders say before the operator reaches
			// for the dropdown.
			std::vector< float > hostOwn;
			for( int j = 0; j < coveredCount; ++j )
				hostOwn.push_back( plugin.GetFloatParameter( covered[ j ] ) );

			// The operator picks a preset.
			plugin.SetFloatParameter( unsigned( presetIndex ), float( preset ) );

			// And now the host says its piece.
			for( int j = 0; j < coveredCount; ++j )
			{
				float back = 0.0f;
				switch( host.kind )
				{
				case Host::Honours:
					back = plugin.GetFloatParameter( covered[ j ] );
					break;
				case Host::Ignores:
					back = hostOwn[ size_t( j ) ];
					break;
				case Host::Quantises:
					back = std::round( plugin.GetFloatParameter( covered[ j ] ) * 1000.0f ) / 1000.0f;
					break;
				}
				plugin.SetFloatParameter( covered[ j ], back );
			}

			const int still = int( std::lround( plugin.GetFloatParameter( unsigned( presetIndex ) ) ) );
			bool ok         = still == preset;

			// Still selected is not enough -- it has to be what renders.
			for( int j = 0; j < coveredCount; ++j )
			{
				const float want = kPresets[ preset - 1 ].v[ j ];
				const float got  = plugin.GetFloatParameter( covered[ j ] );
				ok               = ok && std::fabs( got - want ) <= 1e-4f;
			}

			if( !ok )
			{
				std::printf( "presets %-22s %-22s FAILED (shows %d)\n",
				             host.name, kPresets[ preset - 1 ].name, still );
				++failures;
				continue;
			}

			// An operator turning a covered knob must still drop to Custom -- a
			// preset that cannot be left is no better than one that will not
			// stick. Move it somewhere neither the preset nor the host named.
			const float moved = kPresets[ preset - 1 ].v[ 0 ] > 0.5f ? 0.123f : 0.877f;
			plugin.SetFloatParameter( covered[ 0 ], moved );
			const int after = int( std::lround( plugin.GetFloatParameter( unsigned( presetIndex ) ) ) );
			if( after != 0 )
			{
				std::printf( "presets %-22s %-22s FAILED (an edit left it on %d)\n",
				             host.name, kPresets[ preset - 1 ].name, after );
				++failures;
				continue;
			}

			std::printf( "presets %-22s %-22s ok\n", host.name, kPresets[ preset - 1 ].name );
		}
	}

	std::printf( "%s\n", failures == 0 ? "presets: all ok" : "presets: FAILURES" );
	return failures == 0 ? 0 : 1;
}

//---------------------------------------------------------------------------
/// Prove a Speed or Mutate change does not move the rain.
///
/// The travel either side of the change is read directly rather than comparing
/// rendered frames: the rain is a field of columns on a repeating cycle, so a
/// travel a whole number of cycles away renders identically and two frames
/// would match for entirely the wrong reason. The numbers say it outright.
///
/// Needs no GL, so it runs ahead of the context.
//---------------------------------------------------------------------------
int runSpeedTest()
{
	int failures = 0;

	auto check = [ &failures ]( const char* what, double got, double want, double tol ) {
		const bool ok = std::fabs( got - want ) <= tol;
		std::printf( "speed %-42s got=%-14.4f want=%-14.4f %s\n", what, got, want, ok ? "ok" : "FAILED" );
		if( !ok )
			++failures;
	};

	DownpourPlugin plugin( false );
	plugin.SetClockScaleForTest( 1.0 );//seconds, said out loud rather than inferred

	// An hour in, which is where the old arithmetic hurt most and where a live
	// operator actually is when they reach for the slider.
	double host = 3600.0;
	plugin.SetTime( host );
	plugin.TickClockForTest();

	// Untouched, the anchors must leave the old expressions exactly as they
	// were -- this is what keeps tools/sweep.py and every rendered-frame
	// comparison measuring the same thing they measured before. The plugin's
	// own defaults are asked for rather than written down here: a test that
	// hard-codes them goes quietly wrong the day a default moves.
	{
		const float speed  = SpeedFromParam( plugin.GetFloatParameter( PT_SPEED ) );
		const float mutate = MutateFromParam( plugin.GetFloatParameter( PT_MUTATE ) ) * speed
		                     / kMutateReferenceSpeed;
		check( "travel untouched == clock * speed", plugin.TravelForTest(), host * speed, 1e-2 );
		check( "ticks untouched == clock * mutate", plugin.MutateTicksForTest(), host * mutate, 1e-2 );
	}

	// Speed, then Mutate on its own: Mutate is scaled by Speed, so moving Speed
	// moves the churn too, and anchoring only travel would leave the rain still
	// while every glyph convulsed. Both paths need covering.
	struct Step
	{
		const char* name;
		unsigned int param;
		float slider;
	};
	const Step steps[] = {
		{ "Speed -> 0.10 (slower)", PT_SPEED, 0.10f },
		{ "Speed -> 0.95 (much faster)", PT_SPEED, 0.95f },
		{ "Speed -> 0.00 (stopped)", PT_SPEED, 0.00f },
		{ "Mutate -> 0.90 (churning)", PT_MUTATE, 0.90f },
		{ "Mutate -> 0.00 (still glyphs)", PT_MUTATE, 0.00f },
		{ "Speed -> 0.70 (running again)", PT_SPEED, 0.70f },
	};

	for( const Step& step : steps )
	{
		const float travelBefore = plugin.TravelForTest();
		const float ticksBefore  = plugin.MutateTicksForTest();

		// The same instant, a new setting: nothing about the clock has moved,
		// so nothing about the picture may either.
		plugin.SetFloatParameter( step.param, step.slider );
		plugin.TickClockForTest();

		check( step.name, plugin.TravelForTest(), travelBefore, 1e-2 );
		check( "  and the glyphs do not re-roll", plugin.MutateTicksForTest(), ticksBefore, 1e-2 );

		// And then it must actually run at the new rate.
		const float travelResumed = plugin.TravelForTest();
		const float speed         = SpeedFromParam( plugin.GetFloatParameter( PT_SPEED ) );
		host += 1.0;
		plugin.SetTime( host );
		plugin.TickClockForTest();
		check( "  one second later", plugin.TravelForTest() - travelResumed, speed, 1e-2 );
	}

	// Bar sync is deliberately NOT anchored: its contract is that a cycle lands
	// on the bar line, so it must still be the plain transport product. If the
	// anchor ever leaks into it, beat sync stops meaning anything.
	{
		DownpourPlugin bar( false );
		bar.SetClockScaleForTest( 1.0 );
		bar.SetFloatParameter( PT_SYNC, static_cast< float >( SyncMode::Bar ) );
		bar.SetBeatInfo( 120.0f, 0.25f );//120bpm: a bar is two seconds
		bar.SetTime( 8.0 );
		bar.TickClockForTest();
		const float before = bar.TravelForTest();

		bar.SetFloatParameter( PT_SPEED, 0.95f );
		bar.TickClockForTest();

		const bool jumped = std::fabs( bar.TravelForTest() - before ) > 1e-2;
		std::printf( "speed %-42s %s\n", "Bar sync still re-locks", jumped ? "ok" : "FAILED" );
		if( !jumped )
			++failures;
	}

	std::printf( "%s\n", failures == 0 ? "speed: all ok" : "speed: FAILURES" );
	return failures == 0 ? 0 : 1;
}

int main( int argc, char** argv )
{
	std::string outPath;
	std::string atlasPath;
	std::vector< std::string > settings;
	double time  = 3.0;
	int width    = 1920;
	int height   = 1080;
	bool doList   = false;
	bool doFont   = false;
	bool doRain   = false;
	bool doRead   = false;
	bool doEffect = false;
	std::string sequenceDir;
	std::string cueSheet;
	double seconds = 45.0;
	double fps     = 30.0;

	for( int i = 1; i < argc; ++i )
	{
		const std::string arg = argv[ i ];
		auto value            = [ & ]( const char* what ) -> std::string {
            if( i + 1 >= argc )
            {
                std::fprintf( stderr, "%s wants a value\n", what );
                std::exit( 2 );
            }
            return argv[ ++i ];
		};

		if( arg == "--out" )         outPath = value( "--out" );
		else if( arg == "--atlas" )  atlasPath = value( "--atlas" );
		else if( arg == "--set" )    settings.push_back( value( "--set" ) );
		else if( arg == "--time" )   time = std::strtod( value( "--time" ).c_str(), nullptr );
		else if( arg == "--width" )  width = std::atoi( value( "--width" ).c_str() );
		else if( arg == "--height" ) height = std::atoi( value( "--height" ).c_str() );
		else if( arg == "--presets" )
			return runPresetTest();
		// Ahead of the GL context: this one needs no GPU, so it still runs on a
		// machine that cannot make a context at all.
		else if( arg == "--speed" )
			return runSpeedTest();
		else if( arg == "--list" )   doList = true;
		else if( arg == "--font" )   doFont = true;
		else if( arg == "--rain" )   doRain = true;
		else if( arg == "--readback" ) doRead = true;
		// Drives the effect bundle's class instead of the source's: same rain,
		// over a synthetic clip. Mix and the compositing order have no coverage
		// at all without it, and they are half of what the second bundle is for.
		else if( arg == "--effect" ) doEffect = true;
		else if( arg == "--sequence" ) sequenceDir = value( "--sequence" );
		else if( arg == "--script" )   cueSheet = value( "--script" );
		else if( arg == "--seconds" )  seconds = std::strtod( value( "--seconds" ).c_str(), nullptr );
		else if( arg == "--fps" )      fps = std::strtod( value( "--fps" ).c_str(), nullptr );
		else
		{
			std::fprintf( stderr, "unknown argument '%s'\n", arg.c_str() );
			return 2;
		}
	}

	if( outPath.empty() && atlasPath.empty() && sequenceDir.empty() && !doList && !doFont && !doRain && !doRead )
	{
		std::fprintf( stderr,
		              "usage: dptest [--out PATH] [--atlas PATH] [--list] [--font] [--rain]\n"
		              "              [--speed] [--presets]\n"
		              "              [--readback] [--effect] [--time T] [--width W] [--height H]\n"
		              "              [--set Name=value]...\n"
		              "       dptest --sequence DIR --script CUES [--seconds S] [--fps F]\n"
		              "              [--effect] [--width W] [--height H]\n" );
		return 2;
	}

	CGLContextObj context = createContext();
	if( context == nullptr )
	{
		std::fprintf( stderr, "could not create a GL 4.1 core context\n" );
		return 1;
	}

	int status = 0;

	if( doRain )
	{
		std::printf( "--rain: the shader's cells against Rain.cpp\n" );
		status |= rainCheck();
	}

	if( doRead )
	{
		std::printf( "--readback: a document through the atlas and back\n" );
		status |= readbackCheck();
	}

	if( !sequenceDir.empty() )
		status |= renderSequence( sequenceDir, cueSheet, width, height, seconds, fps, doEffect );

	if( !outPath.empty() || !atlasPath.empty() || doList || doFont )
	{
		DownpourPlugin plugin( doEffect );
		for( const std::string& setting : settings )
		{
			if( !applySetting( plugin, setting ) )
			{
				status |= 1;
				CGLSetCurrentContext( nullptr );
				CGLDestroyContext( context );
				return status;
			}
		}

		Target target      = makeTarget( width, height, false );
		const GLuint input = doEffect ? makeInputTexture( width, height ) : 0;

		if( !render( plugin, target, time, input ) )
		{
			std::fprintf( stderr, "render failed\n" );
			status |= 1;
		}
		else
		{
			if( !outPath.empty() )
			{
				const std::vector< unsigned char > pixels = readBytes( target );
				const std::vector< unsigned char > image  = flipRows( pixels, width, height );
				if( writePng( outPath, width, height, image ) )
					std::printf( "wrote %s (%d x %d at t=%.3f)\n", outPath.c_str(), width, height, time );
				else
				{
					std::fprintf( stderr, "could not write %s\n", outPath.c_str() );
					status |= 1;
				}
			}

			if( !atlasPath.empty() )
				status |= dumpAtlas( plugin, atlasPath );
			if( doList )
				status |= listParameters( plugin );
			if( doFont )
				status |= fontReport( plugin );
		}

		releaseTarget( target );
		if( input != 0 )
			glDeleteTextures( 1, &input );
		plugin.DeInitGL();
	}

	CGLSetCurrentContext( nullptr );
	CGLDestroyContext( context );
	return status;
}
