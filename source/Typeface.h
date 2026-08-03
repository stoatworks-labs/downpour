#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

/**
    Fonts: finding them, and turning one into an atlas.

    ## Finding them

    The font list is built by **walking the operating system's font
    directories** and reading each file's `name` table, rather than by asking
    CoreText on macOS and DirectWrite on Windows. One code path instead of two,
    no framework to link, and -- the part that actually decides it -- the scan
    yields a **file path**, which is what the rasteriser needs anyway. Asking
    the OS for a family name only moves the problem: something still has to turn
    "Helvetica Neue" back into bytes on disk.

    Only the `name` table is read during the scan, located through the table
    directory at the head of the file. Reading whole fonts to get their names
    would mean pulling a couple of hundred megabytes of CJK outlines through
    memory every time a plugin instance is created.

    ## The portability trap

    A composition stores a parameter *value*, so a font chosen from the dropdown
    is stored as **an index into a list that is specific to the machine it was
    chosen on**. Open that composition on a rig with a different set of fonts
    installed and index 47 is a different typeface.

    Downpour handles this by storing the resolved **family name** alongside the
    index in a text parameter, and preferring the name when the two disagree.
    Names survive the trip; indices do not. Where even the name is not present
    on the new machine, the built-in face takes over -- which is the whole
    reason `Glyphs.cpp` carries one.

    ## What lands in the atlas

    Not "the font" -- the **codepoints the stream actually uses**. An English
    passage needs about ninety distinct characters and the katakana set
    forty-eight, so a 961-slot atlas is generous; rasterising a whole CJK font
    would need forty of them. Anything the chosen typeface cannot draw falls
    back to the built-in bitmap for that codepoint, and anything neither can
    draw resolves to the blank slot rather than to a `.notdef` box.

    That fallback is per *codepoint*, not per font, and it is the thing that
    makes "set the font" safe to offer: pick a font with no katakana and you get
    your font for the latin and the built-in face for the katakana, instead of a
    grid of empty rectangles.
*/
namespace downpour
{
/// One installed font file, as found by the scan.
struct FontFile
{
	std::string family; ///< as read from the name table
	std::string path;
	int collectionIndex = 0;///< for .ttc, which face within the file
};

/// Every font found in the OS font directories, sorted by family name and
/// de-duplicated. Scanned once per process and cached -- a plugin instance is
/// created every time an operator drops the effect on a layer, and walking a
/// thousand files each time would be felt.
const std::vector< FontFile >& InstalledFonts();

/// Index of the first font whose family matches `family`, or -1. Case
/// insensitive, because a name that made the round trip through a composition
/// file has no guarantee of case.
int FindFontByFamily( const std::string& family );

/// A loaded typeface, or the built-in face if nothing was loaded.
class Typeface
{
public:
	/// Load a font file. Returns false and leaves the object on the built-in
	/// face if the file will not open or will not parse.
	bool Load( const std::string& path, int collectionIndex = 0 );

	/// Drop back to the built-in face.
	void UseBuiltin();

	bool HasFont() const { return loaded; }
	const std::string& Family() const { return family; }

	/// True if the loaded font draws this codepoint. Always false on the
	/// built-in face -- ask Glyphs.h's BuiltinSlot for that.
	bool Covers( uint32_t codepoint ) const;

	/// Rasterise `codepoints` into `image` (kAtlasPx square, single channel,
	/// bottom row first) and fill `slots` with the codepoint-to-slot mapping.
	///
	/// Slot 0 is left blank and is what an undrawable codepoint maps to.
	/// `missing` counts codepoints that neither this typeface nor the built-in
	/// face could draw.
	void BuildAtlas( const std::vector< uint32_t >& codepoints,
	                 std::vector< uint8_t >& image,
	                 std::unordered_map< uint32_t, int >& slots,
	                 int& fromFont,
	                 int& fromBuiltin,
	                 int& missing ) const;

private:
	std::vector< unsigned char > data;
	std::string family;
	bool loaded = false;

	/// stbtt_fontinfo, kept opaque so stb_truetype.h stays out of this header.
	/// It is a 100-line struct that pulls in the whole implementation, and it
	/// would land in every translation unit that draws a glyph.
	std::vector< unsigned char > info;
};

} // namespace downpour
