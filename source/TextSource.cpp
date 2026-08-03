#include "TextSource.h"

#include <cstdio>
#include <string>
#include <vector>

#include "Corpus.h"
#include "Glyphs.h"

namespace downpour
{
namespace
{
/// Half-width katakana, U+FF66..U+FF9D, minus the punctuation at the bottom of
/// the block. These are the characters the built-in face draws, and the ones a
/// Japanese-capable typeface will have.
std::vector< uint32_t > Katakana()
{
	std::vector< uint32_t > set;
	set.push_back( 0xFF66 );// WO
	for( uint32_t c = 0xFF71; c <= 0xFF9D; ++c )
		set.push_back( c );
	set.push_back( 0xFF70 );// prolonged sound mark
	return set;
}

std::vector< uint32_t > Range( uint32_t first, uint32_t last )
{
	std::vector< uint32_t > set;
	for( uint32_t c = first; c <= last; ++c )
		set.push_back( c );
	return set;
}

void Append( std::vector< uint32_t >& into, const std::vector< uint32_t >& from )
{
	into.insert( into.end(), from.begin(), from.end() );
}

std::vector< uint32_t > Alphabet( CharSet set )
{
	switch( set )
	{
	case CharSet::Katakana:
		return Katakana();

	case CharSet::KatakanaDigits:
	{
		std::vector< uint32_t > all = Katakana();
		Append( all, Range( '0', '9' ) );
		return all;
	}

	case CharSet::Digits:
		return Range( '0', '9' );

	case CharSet::Hex:
	{
		std::vector< uint32_t > all = Range( '0', '9' );
		Append( all, Range( 'A', 'F' ) );
		return all;
	}

	case CharSet::Binary:
		return Range( '0', '1' );

	case CharSet::Ascii:
		// 33 and not 32: the space is left out of a *scattered* set on purpose.
		// A scattered space is a hole in the middle of a trail, and enough of
		// them make the rain look like it is failing to draw rather than like
		// it is sparse. Density is the control for sparseness.
		return Range( 33, 126 );

	case CharSet::Latin:
	{
		std::vector< uint32_t > all = Range( 'A', 'Z' );
		Append( all, Range( 'a', 'z' ) );
		return all;
	}

	default:
		return Katakana();
	}
}

std::string ReadFile( const std::string& path, bool& ok )
{
	ok = false;
	if( path.empty() )
		return {};

	std::FILE* file = std::fopen( path.c_str(), "rb" );
	if( file == nullptr )
		return {};

	std::string contents;
	char buffer[ 65536 ];
	size_t read = 0;
	// Read to the cap and stop. Not fseek-to-end-and-allocate: this opens a
	// path an operator picked, and that path can be a fifty-gigabyte file, a
	// named pipe, or /dev/urandom.
	while( ( read = std::fread( buffer, 1, sizeof( buffer ), file ) ) > 0 )
	{
		contents.append( buffer, read );
		if( contents.size() >= static_cast< size_t >( kMaxDocument ) * 4u )
			break;
	}
	std::fclose( file );

	ok = true;
	return contents;
}

Stream FromText( const std::string& text, Flow flow )
{
	Stream stream;
	stream.flow       = flow;
	stream.codepoints = DecodeUtf8( text );
	if( stream.codepoints.size() > static_cast< size_t >( kMaxDocument ) )
		stream.codepoints.resize( kMaxDocument );
	return stream;
}

Stream FromWork( Work work )
{
	size_t length     = 0;
	const char* text  = WorkText( work, length );
	Stream stream     = FromText( text != nullptr ? std::string( text, length ) : std::string(), Flow::Sequence );
	return stream;
}

Stream FromAlphabet( CharSet set )
{
	Stream stream;
	stream.flow       = Flow::Scatter;
	stream.codepoints = Alphabet( set );
	return stream;
}
} // namespace

//---------------------------------------------------------------------------
// The code generator.
//
// What makes text read as *code* at rain speed is not its grammar -- nobody
// parses a falling column -- it is its character mix. Brackets, semicolons,
// dots, arrows, underscores, hex literals and camelCase are what the eye
// recognises, and prose has almost none of them. So this generates statements
// that would compile if you squinted, and the payoff is in the punctuation
// density rather than in the semantics.
//---------------------------------------------------------------------------
std::string GenerateCode( uint32_t seed, int approximateLength )
{
	static const char* const kTypes[]  = { "int", "void", "float", "uint32_t", "bool", "auto",
	                                       "char", "size_t", "double", "static" };
	static const char* const kNames[]  = { "buffer", "node", "packet", "frame", "state", "index",
	                                       "handle", "stream", "cursor", "matrix", "vector", "chunk",
	                                       "socket", "thread", "kernel", "device", "region", "offset",
	                                       "header", "payload", "channel", "segment" };
	static const char* const kVerbs[]  = { "decode", "resolve", "flush", "commit", "acquire", "map",
	                                       "encode", "advance", "release", "reset", "probe", "align",
	                                       "compute", "dispatch", "collapse", "unwind" };
	static const char* const kOps[]    = { "==", "!=", "<=", ">=", "&&", "||", "<<", ">>", "+=", "->" };
	static const char* const kKeys[]   = { "if", "while", "for", "switch", "return", "else", "break",
	                                       "continue", "case", "goto" };

	const int typeCount = static_cast< int >( sizeof( kTypes ) / sizeof( kTypes[ 0 ] ) );
	const int nameCount = static_cast< int >( sizeof( kNames ) / sizeof( kNames[ 0 ] ) );
	const int verbCount = static_cast< int >( sizeof( kVerbs ) / sizeof( kVerbs[ 0 ] ) );
	const int opCount   = static_cast< int >( sizeof( kOps ) / sizeof( kOps[ 0 ] ) );
	const int keyCount  = static_cast< int >( sizeof( kKeys ) / sizeof( kKeys[ 0 ] ) );

	// The rain's own hash, so the generator is reproducible on any machine and
	// the harness can regenerate exactly what was rendered.
	uint32_t state = seed == 0 ? 1u : seed;
	auto next      = [ &state ]( int modulus ) {
        state = Hash( state );
        return modulus > 0 ? static_cast< int >( state % static_cast< uint32_t >( modulus ) ) : 0;
	};

	std::string out;
	out.reserve( static_cast< size_t >( approximateLength ) + 128 );

	char scratch[ 64 ];

	while( static_cast< int >( out.size() ) < approximateLength )
	{
		switch( next( 6 ) )
		{
		case 0:
			std::snprintf( scratch, sizeof( scratch ), "%s %s_%d = 0x%04X; ",
			               kTypes[ next( typeCount ) ], kNames[ next( nameCount ) ],
			               next( 64 ), next( 65536 ) );
			out += scratch;
			break;

		case 1:
			std::snprintf( scratch, sizeof( scratch ), "if (%s %s %d) { %s(%s); } ",
			               kNames[ next( nameCount ) ], kOps[ next( opCount ) ], next( 512 ),
			               kVerbs[ next( verbCount ) ], kNames[ next( nameCount ) ] );
			out += scratch;
			break;

		case 2:
			std::snprintf( scratch, sizeof( scratch ), "%s->%s[%d] = %s(%s, %d); ",
			               kNames[ next( nameCount ) ], kNames[ next( nameCount ) ], next( 256 ),
			               kVerbs[ next( verbCount ) ], kNames[ next( nameCount ) ], next( 1024 ) );
			out += scratch;
			break;

		case 3:
			std::snprintf( scratch, sizeof( scratch ), "for (int i = 0; i < %d; ++i) { ", next( 4096 ) );
			out += scratch;
			break;

		case 4:
			std::snprintf( scratch, sizeof( scratch ), "%s %s(%s* %s); ",
			               kTypes[ next( typeCount ) ], kVerbs[ next( verbCount ) ],
			               kTypes[ next( typeCount ) ], kNames[ next( nameCount ) ] );
			out += scratch;
			break;

		default:
			std::snprintf( scratch, sizeof( scratch ), "%s %s_%d %s 0x%02X; } ",
			               kKeys[ next( keyCount ) ], kNames[ next( nameCount ) ], next( 32 ),
			               kOps[ next( opCount ) ], next( 256 ) );
			out += scratch;
			break;
		}
	}

	return out;
}

Stream BuildStream( Source source,
                    CharSet set,
                    const std::string& customText,
                    const std::string& filePath )
{
	switch( source )
	{
	case Source::Junk:
	{
		Stream stream = FromAlphabet( set );
		stream.note   = std::string( "junk, " ) + CharSetName( set );
		return stream;
	}

	case Source::Code:
	{
		// Seeded with a constant rather than with the rain's seed: the Seed
		// control is meant to reshuffle which columns fall when, and having it
		// also rewrite the listing makes it impossible to tell those two
		// effects apart while dragging it.
		Stream stream = FromText( GenerateCode( 0x5EED1234u, 24000 ), Flow::Sequence );
		stream.note   = "generated code";
		return stream;
	}

	case Source::Alice:
	case Source::Republic:
	case Source::Hamlet:
	case Source::Discourse:
	{
		const Work work = source == Source::Alice     ? Work::Alice
			: source == Source::Republic              ? Work::Republic
			: source == Source::Hamlet                ? Work::Hamlet
			                                          : Work::Discourse;

		Stream stream = FromWork( work );
		if( !stream.codepoints.empty() )
		{
			stream.note = WorkTitle( work );
			return stream;
		}

		Stream fallback = FromText( GenerateCode( 0x5EED1234u, 24000 ), Flow::Sequence );
		fallback.note     = std::string( "built-in text '" ) + WorkTitle( work )
			+ "' is missing from this build, using generated code";
		fallback.fellBack = true;
		return fallback;
	}

	case Source::CustomText:
	{
		Stream stream = FromText( customText, Flow::Sequence );
		if( !stream.codepoints.empty() )
		{
			stream.note = "custom text";
			return stream;
		}

		Stream fallback = FromAlphabet( CharSet::KatakanaDigits );
		fallback.note     = "Custom Text is empty, using katakana";
		fallback.fellBack = true;
		return fallback;
	}

	case Source::TextFile:
	{
		bool ok            = false;
		const std::string contents = ReadFile( filePath, ok );

		Stream stream = FromText( contents, Flow::Sequence );
		if( ok && !stream.codepoints.empty() )
		{
			stream.note = "file: " + filePath;
			return stream;
		}

		// The ordinary way this happens is a composition saved on one machine
		// and opened on another. Falling back to a built-in work rather than to
		// noise keeps the look of the composition roughly intact until someone
		// re-picks the file.
		Stream fallback = FromWork( Work::Alice );
		fallback.note = filePath.empty()
			? "no text file chosen, using Alice"
			: "could not read '" + filePath + "', using Alice";
		fallback.fellBack = true;
		return fallback;
	}

	default:
		break;
	}

	Stream stream = FromAlphabet( CharSet::KatakanaDigits );
	stream.note   = "unknown source, using katakana";
	stream.fellBack = true;
	return stream;
}

const char* SourceName( Source source )
{
	switch( source )
	{
	case Source::Junk:       return "Junk";
	case Source::Code:       return "Code";
	case Source::Alice:      return "Alice in Wonderland";
	case Source::Republic:   return "Plato - The Cave";
	case Source::Hamlet:     return "Hamlet III.i";
	case Source::Discourse:  return "Descartes - The Cogito";
	case Source::CustomText: return "Custom Text";
	case Source::TextFile:   return "Text File";
	default:                 return "";
	}
}

const char* CharSetName( CharSet set )
{
	switch( set )
	{
	case CharSet::Katakana:       return "Katakana";
	case CharSet::KatakanaDigits: return "Katakana + Digits";
	case CharSet::Digits:         return "Digits";
	case CharSet::Hex:            return "Hex";
	case CharSet::Binary:         return "Binary";
	case CharSet::Ascii:          return "ASCII";
	case CharSet::Latin:          return "Latin";
	default:                      return "";
	}
}

} // namespace downpour
