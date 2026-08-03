#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

/**
    The glyph atlas, and the two things that can fill it.

    Downpour draws characters, and there are two places a character can come
    from: the bitmap face drawn in `GlyphData.cpp`, or any `.ttf`/`.otf`/`.ttc`
    the operator picks, rasterised by `Typeface.cpp`. Everything downstream --
    the rain shader, the document texture, the harness -- deals only in **atlas
    slots**, so it never has to know which of the two it got.

    ## Why there is a built-in face at all

    asciify carries a hand-drawn font and argues in its own `Font.h` against
    carrying a rasteriser; downpour carries both, and the reason is that they
    answer different questions. The rasteriser answers "let me use my font". The
    built-in face answers "what does this look like before I touch anything",
    and, more importantly, "what happens when the font is gone".

    That second one is not hypothetical. A composition saved on one machine
    carries a font *path*, and the next machine is a different rig in a
    different country. A plugin whose picture depends on a path that may not
    resolve needs somewhere to land, and landing on a blank frame in front of an
    audience is not a defensible answer. So the built-in face is the floor:
    slots 0..kBuiltinCount are always drawn, always present, and are what any
    codepoint falls back to when the chosen typeface has nothing for it.

    ## The layout

    One texture, `kAtlasPx` square, single channel, 255 for ink. Slots are laid
    out left to right, bottom to top, `kSlotPx` apart, with the glyph inset by
    `kBorderPx` on every side.

    **The border is load-bearing.** The rain shader samples the atlas with
    `GL_LINEAR` and mipmaps; without a gap, a fetch near a glyph's edge takes
    part of its weight from the neighbouring glyph's ink, and the symptom is a
    faint ghost of the wrong character bleeding in along one side. asciify hit
    the same thing and reached the same answer.

    ## The one convention worth stating once

    `GlyphData.cpp` draws each glyph **top row first**, the way you read it.
    Everything past `ParseArt` is **bottom-up**, to match OpenGL's texture
    origin. The flip happens exactly once, in `ParseArt`, and the failure mode if
    it happens zero or twice is a picture that is upside down in a way that is
    surprisingly easy to look straight past.
*/
namespace downpour
{
/// The bitmap face is drawn on a 12x12 body. Twelve and not asciify's eight
/// because katakana do not survive an 8x8 cell: they are built from three and
/// four stroke groups and at eight rows the crossbars merge into each other.
/// Latin and digits are drawn on a 6x9 body inside the same cell, which is what
/// makes them read as a terminal face rather than as a typeface.
constexpr int kBitmapSize = 12;

/// Rasterisation size for one glyph in the atlas. Everything -- bitmap face and
/// TrueType alike -- lands at this size, so the shader has one code path.
///
/// 64 is not arbitrary. The atlas is built once per font change and sampled
/// with mipmaps, so this has to be big enough for the largest cell anyone will
/// plausibly use (a 12-column grid on a 4K output is 320px) and small enough
/// that a few hundred glyphs fit. Re-rasterising to match the actual cell size
/// would be sharper, but the cell size changes while you drag the Columns
/// slider, and rebuilding an atlas mid-drag stutters where a mip fetch does not.
constexpr int kGlyphPx = 64;

/// One texel of blank on every side. See the class comment -- this is what stops
/// a GL_LINEAR fetch at a glyph edge picking up its neighbour.
constexpr int kBorderPx = 1;
constexpr int kSlotPx   = kGlyphPx + 2 * kBorderPx;// 66

/// 2048/66 = 31 slots per row, 31 rows: 961 slots. Comfortably more than the
/// built-in face plus the distinct codepoints of an English document, which is
/// what sets the requirement -- see TextSource.h.
constexpr int kAtlasPx    = 2048;
constexpr int kAtlasCols  = kAtlasPx / kSlotPx;
constexpr int kAtlasRows  = kAtlasPx / kSlotPx;
constexpr int kAtlasSlots = kAtlasCols * kAtlasRows;

/// Slot 0 is always blank. Not a convention for tidiness: it is what an unknown
/// codepoint, an inactive cell and a gap between drops all resolve to, and
/// having exactly one of those means the shader never needs a branch to ask
/// whether it should be drawing anything.
constexpr int kBlankSlot = 0;

/// A glyph of the built-in face: a binary bitmap, row 0 at the **bottom**.
struct Bitmap
{
	uint32_t codepoint = 0;
	uint16_t rows[ kBitmapSize ] = { 0 };///< bit c of rows[r] is column c from the left

	bool Ink( int col, int row ) const
	{
		return ( rows[ row ] >> col ) & 1u;
	}
};

/// The raw drawn form, exactly as GlyphData.cpp writes it: twelve rows of twelve
/// characters, **top row first**, '#' for ink and anything else for paper.
struct BitmapArt
{
	uint32_t codepoint;
	const char* rows[ kBitmapSize ];
};

/// The face as drawn. Defined in GlyphData.cpp and nowhere else.
const BitmapArt* FaceArt( size_t& count );

/// The face, parsed and the right way up. A glyph's index here is its atlas slot
/// for the whole life of the process.
const std::vector< Bitmap >& Face();

/// Atlas slot for a codepoint in the built-in face, or -1 if it does not draw it.
int BuiltinSlot( uint32_t codepoint );

/// How many slots the built-in face occupies. Slots from here up are free for a
/// rasterised typeface to claim.
int BuiltinCount();

/// Split a UTF-8 string into codepoints. Malformed bytes are skipped rather than
/// guessed at: this parses whatever an operator typed into a text field in
/// somebody else's application, and whatever was in a file they picked, so it
/// has to be unable to fail.
std::vector< uint32_t > DecodeUtf8( const std::string& text );

/// Encode one codepoint back to UTF-8. Used by the harness to report what it
/// read back out of a rendered frame.
std::string EncodeUtf8( uint32_t codepoint );

/// Draw the built-in face's glyph for `slot` into an atlas image at its slot
/// position. `image` is kAtlasPx * kAtlasPx, single channel, bottom row first.
///
/// The bitmap is scaled up by an integer factor with no smoothing, on purpose.
/// A 12x12 bitmap smoothed up to 64px is a blurry 12x12 bitmap; left blocky it
/// is a bitmap face, which is a thing a terminal actually looked like.
void DrawBitmapIntoAtlas( std::vector< uint8_t >& image, int slot, const Bitmap& glyph );

/// Where a slot lives in the atlas, in texels, origin bottom-left, not counting
/// the border.
void SlotOrigin( int slot, int& x, int& y );

} // namespace downpour
