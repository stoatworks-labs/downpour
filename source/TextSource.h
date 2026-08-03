#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "Rain.h"

/**
    Where the characters come from.

    Everything the rain draws is a **stream**: a flat list of Unicode code
    points. The rain does not know or care whether that list is an alphabet to
    draw from at random or a book to read in order -- it is handed the list and
    a `Flow` saying which, and `Rain.cpp` indexes it accordingly.

    Collapsing "character set" and "document" into one thing is what keeps this
    from being two plugins wearing a trenchcoat. There is one texture, one
    length uniform, and one place where a codepoint becomes an atlas slot.

    ## The two flows, and why the source decides

    - **Junk** hands over an alphabet and asks for `Flow::Scatter`. Each cell
      draws independently, so a column reads as noise. This is the code rain.
    - **Everything else** hands over a document and asks for `Flow::Sequence`,
      which reads consecutive characters down a column and consecutive columns
      across the frame. That is what makes a loaded text *play out* instead of
      merely supplying letters.

    The source picks the flow rather than the operator, because the two
    combinations that this rules out are both traps. Scattering a book gives
    you a very expensive way to generate noise, and sequencing a six-character
    alphabet gives you a marquee that repeats every six cells and looks broken.
*/
namespace downpour
{
/// What to draw. Serialised into compositions by value, so entries may be
/// appended but not reordered.
enum class Source : int
{
	Junk = 0,   ///< random characters from the selected set
	Code,       ///< generated tokens that read as source code
	Alice,      ///< Carroll
	Republic,   ///< Plato -- the cave
	Hamlet,     ///< Shakespeare
	Discourse,  ///< Descartes -- the cogito
	CustomText, ///< whatever was typed into the Custom Text field
	TextFile,   ///< a .txt chosen with the file picker

	Count
};

/// The alphabet Junk draws from. Only read when Source is Junk.
enum class CharSet : int
{
	Katakana = 0,   ///< half-width katakana: the code rain
	KatakanaDigits, ///< katakana and 0-9, which is what the film actually used
	Digits,
	Hex,
	Binary,
	Ascii,          ///< every printable ASCII character
	Latin,          ///< A-Z a-z

	Count
};

/// A resolved stream, plus what happened while resolving it.
struct Stream
{
	std::vector< uint32_t > codepoints;
	Flow flow = Flow::Scatter;

	/// One line for the log. Says which source was used and, when a fallback
	/// happened, which one and why -- a picture that silently came from the
	/// wrong place is the failure this exists to make visible.
	std::string note;

	/// True when the requested source could not be used at all.
	bool fellBack = false;
};

/// The largest document accepted from disk, in code points.
///
/// A million is about eleven megabytes of stream texture and rather more than
/// anyone will watch: at a legible rain speed a screenful is a few thousand
/// characters. The cap exists because the alternative to a cap is an operator
/// dropping a 200 MB log file on it mid-show and finding out what happens.
constexpr int kMaxDocument = 1000000;

/// Build the stream. `customText` is read only for Source::CustomText,
/// `filePath` only for Source::TextFile.
///
/// **Never fails.** Every path that cannot produce what was asked for falls
/// back -- to the built-in code generator for a missing file, to the katakana
/// set for an empty custom string -- and says so in `note`. A generator that
/// returns nothing renders a black frame, and a black frame in front of an
/// audience is indistinguishable from a crash.
Stream BuildStream( Source source,
                    CharSet set,
                    const std::string& customText,
                    const std::string& filePath );

/// Name as shown in the host's dropdown.
const char* SourceName( Source source );
const char* CharSetName( CharSet set );

/// The code generator, exposed so the harness can measure it without going
/// through a file. Deterministic in `seed`: the same seed is the same listing,
/// which is what lets `--readback` compare a rendered frame against it.
std::string GenerateCode( uint32_t seed, int approximateLength );

} // namespace downpour
