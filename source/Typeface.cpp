#include "Typeface.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <set>

#include "Diag.h"
#include "Glyphs.h"

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "stb_truetype.h"

#if defined( _WIN32 )
	#include <windows.h>
#else
	#include <dirent.h>
	#include <sys/stat.h>
#endif

namespace downpour
{
namespace
{
//---------------------------------------------------------------------------
// A very small slice of the OpenType container, just enough to find the name
// table without reading the whole file. See Typeface.h for why.
//---------------------------------------------------------------------------
uint16_t ReadU16( const unsigned char* p )
{
	return static_cast< uint16_t >( ( p[ 0 ] << 8 ) | p[ 1 ] );
}

uint32_t ReadU32( const unsigned char* p )
{
	return ( static_cast< uint32_t >( p[ 0 ] ) << 24 ) | ( static_cast< uint32_t >( p[ 1 ] ) << 16 )
		| ( static_cast< uint32_t >( p[ 2 ] ) << 8 ) | static_cast< uint32_t >( p[ 3 ] );
}

bool ReadAt( std::FILE* file, long offset, void* into, size_t bytes )
{
	if( std::fseek( file, offset, SEEK_SET ) != 0 )
		return false;
	return std::fread( into, 1, bytes, file ) == bytes;
}

/// UTF-16BE, as the Windows platform stores names, reduced to ASCII. Family
/// names outside ASCII exist, but this is a dropdown label -- it has to survive
/// a round trip through an FFGL parameter name, which is a `char*`.
std::string DecodeNameUtf16( const std::vector< unsigned char >& raw )
{
	std::string out;
	for( size_t i = 0; i + 1 < raw.size(); i += 2 )
	{
		const uint16_t unit = ReadU16( raw.data() + i );
		out += ( unit >= 32 && unit < 127 ) ? static_cast< char >( unit ) : '?';
	}
	return out;
}

std::string DecodeNameAscii( const std::vector< unsigned char >& raw )
{
	std::string out;
	for( unsigned char c : raw )
		out += ( c >= 32 && c < 127 ) ? static_cast< char >( c ) : '?';
	return out;
}

/// Family name of the face at `faceOffset`, read from the name table alone.
std::string ReadFamilyName( std::FILE* file, uint32_t faceOffset )
{
	unsigned char header[ 12 ];
	if( !ReadAt( file, static_cast< long >( faceOffset ), header, sizeof( header ) ) )
		return {};

	const uint16_t tableCount = ReadU16( header + 4 );
	if( tableCount == 0 || tableCount > 512 )
		return {};

	std::vector< unsigned char > directory( static_cast< size_t >( tableCount ) * 16u );
	if( !ReadAt( file, static_cast< long >( faceOffset ) + 12, directory.data(), directory.size() ) )
		return {};

	uint32_t nameOffset = 0;
	uint32_t nameLength = 0;
	for( uint16_t i = 0; i < tableCount; ++i )
	{
		const unsigned char* record = directory.data() + static_cast< size_t >( i ) * 16u;
		if( std::memcmp( record, "name", 4 ) == 0 )
		{
			nameOffset = ReadU32( record + 8 );
			nameLength = ReadU32( record + 12 );
			break;
		}
	}

	if( nameOffset == 0 || nameLength < 6 || nameLength > 1u << 20 )
		return {};

	std::vector< unsigned char > table( nameLength );
	if( !ReadAt( file, static_cast< long >( nameOffset ), table.data(), table.size() ) )
		return {};

	const uint16_t recordCount = ReadU16( table.data() + 2 );
	const uint16_t storage     = ReadU16( table.data() + 4 );

	std::string best;
	int bestScore = -1;

	for( uint16_t i = 0; i < recordCount; ++i )
	{
		const size_t at = 6u + static_cast< size_t >( i ) * 12u;
		if( at + 12 > table.size() )
			break;

		const unsigned char* record = table.data() + at;
		const uint16_t platform     = ReadU16( record + 0 );
		const uint16_t language     = ReadU16( record + 4 );
		const uint16_t nameId       = ReadU16( record + 6 );
		const uint16_t length       = ReadU16( record + 8 );
		const uint16_t offset       = ReadU16( record + 10 );

		// 16 is the typographic family ("Helvetica Neue"); 1 is the legacy
		// family, which splits weights into separate families ("Helvetica Neue
		// Light"). Prefer 16 where a font has it so the dropdown lists a family
		// once rather than nine times.
		if( nameId != 1 && nameId != 16 )
			continue;

		const size_t start = static_cast< size_t >( storage ) + offset;
		if( start + length > table.size() )
			continue;

		std::vector< unsigned char > raw( table.begin() + start, table.begin() + start + length );
		const std::string decoded = ( platform == 3 || platform == 0 ) ? DecodeNameUtf16( raw )
		                                                              : DecodeNameAscii( raw );
		if( decoded.empty() )
			continue;

		// A name record can be in any script, and a font that ships an Arabic or
		// a Chinese family name usually ships an English one alongside it.
		// Taking whichever came first put a run of families called "????" at the
		// top of the dropdown -- the decoder is doing the only thing it can with
		// a script it cannot represent in a `char*` parameter name, so the fix
		// is to prefer the record that survives the trip.
		const size_t unknown = static_cast< size_t >( std::count( decoded.begin(), decoded.end(), '?' ) );
		if( unknown * 4 > decoded.size() )
			continue;

		// 0x409 is Windows US English; language 0 on the Mac platform is English.
		const bool english = ( platform == 3 && language == 0x409 ) || ( platform == 1 && language == 0 );

		const int score = ( nameId == 16 ? 4 : 0 ) + ( english ? 2 : 0 ) + ( platform == 3 ? 1 : 0 );
		if( score > bestScore )
		{
			bestScore = score;
			best      = decoded;
		}
	}

	return best;
}

/// Every face in one font file. A .ttc holds several.
void ScanFontFile( const std::string& path, std::vector< FontFile >& into )
{
	std::FILE* file = std::fopen( path.c_str(), "rb" );
	if( file == nullptr )
		return;

	unsigned char tag[ 12 ];
	if( std::fread( tag, 1, sizeof( tag ), file ) != sizeof( tag ) )
	{
		std::fclose( file );
		return;
	}

	std::vector< uint32_t > faceOffsets;

	if( std::memcmp( tag, "ttcf", 4 ) == 0 )
	{
		const uint32_t faceCount = ReadU32( tag + 8 );
		if( faceCount > 0 && faceCount < 4096 )
		{
			std::vector< unsigned char > offsets( static_cast< size_t >( faceCount ) * 4u );
			if( ReadAt( file, 12, offsets.data(), offsets.size() ) )
			{
				for( uint32_t i = 0; i < faceCount; ++i )
					faceOffsets.push_back( ReadU32( offsets.data() + static_cast< size_t >( i ) * 4u ) );
			}
		}
	}
	else
	{
		const uint32_t version = ReadU32( tag );
		// 0x00010000 is TrueType outlines, 'OTTO' is CFF, 'true' is an old Mac
		// TrueType. Anything else is not a font we can rasterise.
		if( version == 0x00010000u || version == 0x4F54544Fu || version == 0x74727565u )
			faceOffsets.push_back( 0 );
	}

	for( size_t i = 0; i < faceOffsets.size(); ++i )
	{
		const std::string family = ReadFamilyName( file, faceOffsets[ i ] );
		if( family.empty() )
			continue;

		// macOS ships around a hundred internal faces whose family name starts
		// with a dot -- ".Aqua Kana", ".Apple Color Emoji UI" and friends. They
		// are UI machinery rather than typefaces, several will not rasterise,
		// and listing them buries the real fonts. Apple's own font menus hide
		// them on exactly this rule.
		if( family[ 0 ] == '.' )
			continue;

		FontFile entry;
		entry.family          = family;
		entry.path            = path;
		entry.collectionIndex = static_cast< int >( i );
		into.push_back( entry );
	}

	std::fclose( file );
}

bool HasFontExtension( const std::string& name )
{
	if( name.size() < 5 )
		return false;
	std::string tail = name.substr( name.size() - 4 );
	for( char& c : tail )
		c = static_cast< char >( std::tolower( static_cast< unsigned char >( c ) ) );
	return tail == ".ttf" || tail == ".otf" || tail == ".ttc" || tail == ".otc";
}

void ScanDirectory( const std::string& directory, std::vector< FontFile >& into, int depth )
{
	// Two levels. macOS keeps a few fonts one folder down (Supplemental, and
	// per-family folders); nothing useful is deeper, and an unbounded walk over
	// a home directory is how a plugin constructor becomes a hang.
	if( depth > 2 )
		return;

#if defined( _WIN32 )
	const std::string pattern = directory + "\\*";
	WIN32_FIND_DATAA found {};
	HANDLE handle = FindFirstFileA( pattern.c_str(), &found );
	if( handle == INVALID_HANDLE_VALUE )
		return;

	do
	{
		const std::string name = found.cFileName;
		if( name == "." || name == ".." )
			continue;

		const std::string full = directory + "\\" + name;
		if( ( found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY ) != 0 )
			ScanDirectory( full, into, depth + 1 );
		else if( HasFontExtension( name ) )
			ScanFontFile( full, into );
	} while( FindNextFileA( handle, &found ) );

	FindClose( handle );
#else
	DIR* dir = opendir( directory.c_str() );
	if( dir == nullptr )
		return;

	while( struct dirent* entry = readdir( dir ) )
	{
		const std::string name = entry->d_name;
		if( name.empty() || name[ 0 ] == '.' )
			continue;

		const std::string full = directory + "/" + name;

		struct stat info {};
		if( stat( full.c_str(), &info ) != 0 )
			continue;

		if( S_ISDIR( info.st_mode ) )
			ScanDirectory( full, into, depth + 1 );
		else if( S_ISREG( info.st_mode ) && HasFontExtension( name ) )
			ScanFontFile( full, into );
	}

	closedir( dir );
#endif
}

std::vector< std::string > FontDirectories()
{
	std::vector< std::string > directories;

#if defined( _WIN32 )
	char windows[ MAX_PATH ] {};
	if( GetWindowsDirectoryA( windows, MAX_PATH ) != 0 )
		directories.push_back( std::string( windows ) + "\\Fonts" );

	const char* local = std::getenv( "LOCALAPPDATA" );
	if( local != nullptr )
		directories.push_back( std::string( local ) + "\\Microsoft\\Windows\\Fonts" );
#else
	const char* home = std::getenv( "HOME" );
	if( home != nullptr )
		directories.push_back( std::string( home ) + "/Library/Fonts" );
	directories.push_back( "/Library/Fonts" );
	directories.push_back( "/System/Library/Fonts" );
#endif

	return directories;
}

std::string Lowered( const std::string& text )
{
	std::string out = text;
	for( char& c : out )
		c = static_cast< char >( std::tolower( static_cast< unsigned char >( c ) ) );
	return out;
}

stbtt_fontinfo* AsFontInfo( std::vector< unsigned char >& storage )
{
	return storage.empty() ? nullptr : reinterpret_cast< stbtt_fontinfo* >( storage.data() );
}

const stbtt_fontinfo* AsFontInfo( const std::vector< unsigned char >& storage )
{
	return storage.empty() ? nullptr : reinterpret_cast< const stbtt_fontinfo* >( storage.data() );
}
} // namespace

const std::vector< FontFile >& InstalledFonts()
{
	static const std::vector< FontFile > fonts = [] {
		std::vector< FontFile > found;
		for( const std::string& directory : FontDirectories() )
			ScanDirectory( directory, found, 0 );

		std::sort( found.begin(), found.end(), []( const FontFile& a, const FontFile& b ) {
			const std::string left  = Lowered( a.family );
			const std::string right = Lowered( b.family );
			if( left != right )
				return left < right;
			return a.path < b.path;
		} );

		// One entry per family. A family with four weights is four files with
		// the same typographic name, and listing it four times makes the
		// dropdown useless without telling anyone anything.
		found.erase( std::unique( found.begin(), found.end(),
		                          []( const FontFile& a, const FontFile& b ) {
			                          return Lowered( a.family ) == Lowered( b.family );
		                          } ),
		             found.end() );

		diag::info( "font scan found " + std::to_string( found.size() ) + " families" );
		return found;
	}();

	return fonts;
}

int FindFontByFamily( const std::string& family )
{
	if( family.empty() )
		return -1;

	const std::string wanted        = Lowered( family );
	const std::vector< FontFile >& fonts = InstalledFonts();
	for( size_t i = 0; i < fonts.size(); ++i )
	{
		if( Lowered( fonts[ i ].family ) == wanted )
			return static_cast< int >( i );
	}
	return -1;
}

void Typeface::UseBuiltin()
{
	data.clear();
	info.clear();
	family.clear();
	loaded = false;
}

bool Typeface::Load( const std::string& path, int collectionIndex )
{
	UseBuiltin();

	if( path.empty() )
		return false;

	std::FILE* file = std::fopen( path.c_str(), "rb" );
	if( file == nullptr )
	{
		diag::warn( "font will not open: " + path );
		return false;
	}

	std::fseek( file, 0, SEEK_END );
	const long size = std::ftell( file );
	std::fseek( file, 0, SEEK_SET );

	// A font over 64 MB is a pan-CJK collection, and loading one to draw ninety
	// latin characters is not a trade worth making inside somebody else's
	// process.
	if( size <= 0 || size > 64L * 1024L * 1024L )
	{
		std::fclose( file );
		diag::warn( "font is an implausible size, ignoring: " + path );
		return false;
	}

	data.resize( static_cast< size_t >( size ) );
	const size_t read = std::fread( data.data(), 1, data.size(), file );
	std::fclose( file );

	if( read != data.size() )
	{
		UseBuiltin();
		return false;
	}

	info.resize( sizeof( stbtt_fontinfo ) );
	stbtt_fontinfo* font = AsFontInfo( info );

	const int offset = stbtt_GetFontOffsetForIndex( data.data(), collectionIndex );
	if( offset < 0 || stbtt_InitFont( font, data.data(), offset ) == 0 )
	{
		UseBuiltin();
		diag::warn( "font will not parse: " + path );
		return false;
	}

	loaded = true;

	// Re-open to read the name table rather than keeping the handle: the load
	// path above needs the file closed before it can be sure the whole thing is
	// in `data`, and a family name is worth one extra open.
	std::FILE* named = std::fopen( path.c_str(), "rb" );
	if( named != nullptr )
	{
		family = ReadFamilyName( named, static_cast< uint32_t >( offset ) );
		std::fclose( named );
	}

	diag::info( "font loaded: " + ( family.empty() ? path : family ) );
	return true;
}

bool Typeface::Covers( uint32_t codepoint ) const
{
	if( !loaded )
		return false;

	const stbtt_fontinfo* font = AsFontInfo( info );
	if( font == nullptr )
		return false;

	// Glyph 0 is .notdef by definition, and a font reports it for anything it
	// does not have. Treating that as coverage is exactly how a picture becomes
	// a grid of empty rectangles.
	return stbtt_FindGlyphIndex( font, static_cast< int >( codepoint ) ) != 0;
}

void Typeface::BuildAtlas( const std::vector< uint32_t >& codepoints,
                           std::vector< uint8_t >& image,
                           std::unordered_map< uint32_t, int >& slots,
                           int& fromFont,
                           int& fromBuiltin,
                           int& missing ) const
{
	image.assign( static_cast< size_t >( kAtlasPx ) * kAtlasPx, 0 );
	slots.clear();
	fromFont    = 0;
	fromBuiltin = 0;
	missing     = 0;

	// Slot 0 stays blank. Everything undrawable resolves here, which is what
	// lets the shader draw "nothing" without a branch.
	int nextSlot = kBlankSlot + 1;

	const stbtt_fontinfo* font = AsFontInfo( info );

	// Scale so that the font's ascent-to-descent fills the glyph box. Not
	// stbtt_ScaleForPixelHeight, which scales the *em*: an em box is nominal
	// and most faces overflow it, so scaling by it clips the tops off capitals
	// in about a third of the fonts on this machine.
	float scale     = 0.0f;
	int ascent      = 0;
	int descent     = 0;
	int lineGap     = 0;
	if( loaded && font != nullptr )
	{
		stbtt_GetFontVMetrics( font, &ascent, &descent, &lineGap );
		const int span = ascent - descent;
		scale = span > 0 ? static_cast< float >( kGlyphPx ) / static_cast< float >( span ) : 0.0f;
	}

	std::set< uint32_t > seen;

	for( uint32_t codepoint : codepoints )
	{
		if( slots.find( codepoint ) != slots.end() )
			continue;
		if( !seen.insert( codepoint ).second )
			continue;

		if( nextSlot >= kAtlasSlots )
		{
			slots[ codepoint ] = kBlankSlot;
			++missing;
			continue;
		}

		//-----------------------------------------------------------------
		// The chosen typeface first.
		//-----------------------------------------------------------------
		if( loaded && font != nullptr && scale > 0.0f && Covers( codepoint ) )
		{
			int width = 0, height = 0, offsetX = 0, offsetY = 0;
			unsigned char* bitmap = stbtt_GetCodepointBitmap( font, scale, scale,
			                                                  static_cast< int >( codepoint ),
			                                                  &width, &height, &offsetX, &offsetY );

			// A blank raster is the normal outcome for a space, and it is a
			// glyph rather than a failure: the slot stays claimed and blank.
			if( bitmap != nullptr && width > 0 && height > 0 )
			{
				int originX = 0, originY = 0;
				SlotOrigin( nextSlot, originX, originY );

				// Centre it in the box. Honouring the font's own bearings would
				// be right for setting a line of text and is wrong here: every
				// glyph gets its own cell, and left-aligning them by bearing
				// makes a column of rain visibly wobble.
				const int insetX = ( kGlyphPx - std::min( width, kGlyphPx ) ) / 2;
				const int insetY = ( kGlyphPx - std::min( height, kGlyphPx ) ) / 2;

				for( int y = 0; y < height && y < kGlyphPx; ++y )
				{
					// stb rasterises top row first; the atlas is bottom row
					// first. This is the only flip on the TrueType path.
					const int target = originY + insetY + ( std::min( height, kGlyphPx ) - 1 - y );
					uint8_t* line    = image.data() + static_cast< size_t >( target ) * kAtlasPx;
					for( int x = 0; x < width && x < kGlyphPx; ++x )
						line[ originX + insetX + x ] = bitmap[ y * width + x ];
				}
			}

			if( bitmap != nullptr )
				stbtt_FreeBitmap( bitmap, nullptr );

			slots[ codepoint ] = nextSlot++;
			++fromFont;
			continue;
		}

		//-----------------------------------------------------------------
		// Then the built-in face, per codepoint. This is the fallback that
		// makes picking a font with no katakana safe.
		//-----------------------------------------------------------------
		const int builtin = BuiltinSlot( codepoint );
		if( builtin >= 0 )
		{
			DrawBitmapIntoAtlas( image, nextSlot, Face()[ static_cast< size_t >( builtin ) ] );
			slots[ codepoint ] = nextSlot++;
			++fromBuiltin;
			continue;
		}

		slots[ codepoint ] = kBlankSlot;
		++missing;
	}
}

} // namespace downpour
