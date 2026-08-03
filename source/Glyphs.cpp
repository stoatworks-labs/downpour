#include "Glyphs.h"

#include <algorithm>
#include <unordered_map>

namespace downpour
{
namespace
{
/// Turn one drawn glyph the right way up. This is the only place the flip
/// happens -- see the note in Glyphs.h about doing it zero or two times.
Bitmap ParseArt( const BitmapArt& art )
{
	Bitmap glyph;
	glyph.codepoint = art.codepoint;

	for( int drawnRow = 0; drawnRow < kBitmapSize; ++drawnRow )
	{
		const char* line = art.rows[ drawnRow ];
		if( line == nullptr )
			continue;

		// Row 0 of the drawing is the top; row 0 of the bitmap is the bottom.
		const int row = kBitmapSize - 1 - drawnRow;

		uint16_t bits = 0;
		for( int col = 0; col < kBitmapSize && line[ col ] != '\0'; ++col )
		{
			// Anything that is not ink is paper. Writing the art with '.' for
			// paper reads far better than spaces, and a trailing run of paper
			// can then simply be left off the end of a line.
			if( line[ col ] == '#' )
				bits |= static_cast< uint16_t >( 1u << col );
		}
		glyph.rows[ row ] = bits;
	}

	return glyph;
}

const std::vector< Bitmap >& BuildFace()
{
	static const std::vector< Bitmap > face = [] {
		size_t count = 0;
		const BitmapArt* art = FaceArt( count );

		std::vector< Bitmap > parsed;
		parsed.reserve( count );
		for( size_t i = 0; i < count; ++i )
			parsed.push_back( ParseArt( art[ i ] ) );
		return parsed;
	}();
	return face;
}

const std::unordered_map< uint32_t, int >& BuildIndex()
{
	static const std::unordered_map< uint32_t, int > index = [] {
		std::unordered_map< uint32_t, int > map;
		const std::vector< Bitmap >& face = BuildFace();
		for( size_t i = 0; i < face.size(); ++i )
		{
			// First declaration wins. A codepoint drawn twice is a mistake in
			// GlyphData.cpp rather than a choice, and `dptest --font` reports it
			// -- but silently taking the first keeps the atlas well formed in
			// the meantime instead of making it depend on map insertion order.
			map.emplace( face[ i ].codepoint, static_cast< int >( i ) );
		}
		return map;
	}();
	return index;
}
} // namespace

const std::vector< Bitmap >& Face()
{
	return BuildFace();
}

int BuiltinCount()
{
	return static_cast< int >( BuildFace().size() );
}

int BuiltinSlot( uint32_t codepoint )
{
	const auto& index = BuildIndex();
	const auto found  = index.find( codepoint );
	return found == index.end() ? -1 : found->second;
}

void SlotOrigin( int slot, int& x, int& y )
{
	const int col = slot % kAtlasCols;
	const int row = slot / kAtlasCols;
	x = col * kSlotPx + kBorderPx;
	y = row * kSlotPx + kBorderPx;
}

void DrawBitmapIntoAtlas( std::vector< uint8_t >& image, int slot, const Bitmap& glyph )
{
	if( slot < 0 || slot >= kAtlasSlots )
		return;
	if( image.size() < static_cast< size_t >( kAtlasPx ) * kAtlasPx )
		return;

	int originX = 0, originY = 0;
	SlotOrigin( slot, originX, originY );

	// Integer scale, nearest neighbour, centred in the slot. 64 / 12 = 5, so the
	// glyph occupies 60 of the 64 texels with two spare either side; keeping the
	// factor whole is what stops the strokes of a bitmap face coming out uneven
	// widths, which is the thing the eye actually notices.
	const int scale = kGlyphPx / kBitmapSize;
	const int drawn = scale * kBitmapSize;
	const int inset = ( kGlyphPx - drawn ) / 2;

	for( int row = 0; row < kBitmapSize; ++row )
	{
		for( int col = 0; col < kBitmapSize; ++col )
		{
			if( !glyph.Ink( col, row ) )
				continue;

			for( int dy = 0; dy < scale; ++dy )
			{
				const int y = originY + inset + row * scale + dy;
				uint8_t* line = image.data() + static_cast< size_t >( y ) * kAtlasPx;
				for( int dx = 0; dx < scale; ++dx )
					line[ originX + inset + col * scale + dx ] = 255;
			}
		}
	}
}

std::vector< uint32_t > DecodeUtf8( const std::string& text )
{
	std::vector< uint32_t > codepoints;
	codepoints.reserve( text.size() );

	size_t i = 0;
	while( i < text.size() )
	{
		const unsigned char lead = static_cast< unsigned char >( text[ i ] );

		int extra        = 0;
		uint32_t value   = 0;
		uint32_t minimum = 0;

		if( lead < 0x80 )
		{
			codepoints.push_back( lead );
			++i;
			continue;
		}
		else if( ( lead & 0xE0 ) == 0xC0 )
		{
			extra   = 1;
			value   = lead & 0x1Fu;
			minimum = 0x80;
		}
		else if( ( lead & 0xF0 ) == 0xE0 )
		{
			extra   = 2;
			value   = lead & 0x0Fu;
			minimum = 0x800;
		}
		else if( ( lead & 0xF8 ) == 0xF0 )
		{
			extra   = 3;
			value   = lead & 0x07u;
			minimum = 0x10000;
		}
		else
		{
			// A continuation byte or an invalid lead. Skip it. Guessing at what
			// was meant is how a decoder turns a corrupt file into a plausible
			// wrong answer.
			++i;
			continue;
		}

		if( i + extra >= text.size() )
			break;

		bool valid = true;
		for( int k = 1; k <= extra; ++k )
		{
			const unsigned char continuation = static_cast< unsigned char >( text[ i + k ] );
			if( ( continuation & 0xC0 ) != 0x80 )
			{
				valid = false;
				break;
			}
			value = ( value << 6 ) | ( continuation & 0x3Fu );
		}

		if( !valid )
		{
			++i;
			continue;
		}

		i += extra + 1;

		// Overlong encodings, surrogates and out-of-range values are all dropped
		// rather than passed on: none of them can name a glyph, and letting them
		// through only moves the problem to whatever indexes the atlas.
		if( value < minimum || value > 0x10FFFF )
			continue;
		if( value >= 0xD800 && value <= 0xDFFF )
			continue;

		codepoints.push_back( value );
	}

	return codepoints;
}

std::string EncodeUtf8( uint32_t codepoint )
{
	std::string out;
	if( codepoint < 0x80 )
	{
		out += static_cast< char >( codepoint );
	}
	else if( codepoint < 0x800 )
	{
		out += static_cast< char >( 0xC0 | ( codepoint >> 6 ) );
		out += static_cast< char >( 0x80 | ( codepoint & 0x3F ) );
	}
	else if( codepoint < 0x10000 )
	{
		out += static_cast< char >( 0xE0 | ( codepoint >> 12 ) );
		out += static_cast< char >( 0x80 | ( ( codepoint >> 6 ) & 0x3F ) );
		out += static_cast< char >( 0x80 | ( codepoint & 0x3F ) );
	}
	else
	{
		out += static_cast< char >( 0xF0 | ( codepoint >> 18 ) );
		out += static_cast< char >( 0x80 | ( ( codepoint >> 12 ) & 0x3F ) );
		out += static_cast< char >( 0x80 | ( ( codepoint >> 6 ) & 0x3F ) );
		out += static_cast< char >( 0x80 | ( codepoint & 0x3F ) );
	}
	return out;
}

} // namespace downpour
