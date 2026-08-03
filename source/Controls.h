#pragma once

#include <cstdint>

/**
    Host parameters, and what they mean.

    **Every numeric parameter this plugin declares is a plain 0..1 float**, even
    where it stands for a column count, a seed or rows per second. That is not a
    style preference. `CFFGLPluginManager::SetParamInfo` clamps an
    `FF_TYPE_STANDARD` default into 0..1 *before* returning, and `SetParamRange`
    can only be called afterwards because it finds the parameter by id -- so a
    parameter declared in rows-per-second cannot declare a default in
    rows-per-second. There is no `SetParamDefault`. A default of 8 becomes 1,
    silently, and the plugin starts up wrong in a way no build step notices.

    So the range lives here, in the conversion, and the host only ever sees
    0..1.

    Curves rather than straight lines wherever the useful part of a range is
    bunched at one end. Columns is the clear case: the difference between 20 and
    30 columns is a different-looking effect, and the difference between 180 and
    190 is nothing at all, so a linear slider would spend four fifths of its
    travel on choices nobody makes.
*/
namespace downpour
{
/// Parameter ids. The declaration order in Downpour.cpp is the order they
/// appear in the host, and the groups depend on consecutive ids staying
/// consecutive -- SetParamGroup collapses *runs* of same-group parameters, so
/// reordering these silently splits a group into two.
enum ParamId : unsigned int
{
	// Rain
	PT_SPEED = 0,
	PT_DIRECTION,
	PT_COLUMNS,
	PT_TRAIL,
	PT_DENSITY,
	PT_MUTATE,
	PT_FALLOFF,
	PT_SEED,

	// Text
	PT_SOURCE,
	PT_CHARSET,
	PT_CUSTOM_TEXT,
	PT_TEXT_FILE,
	PT_MIRROR,

	// Font
	PT_FONT,
	PT_FONT_FILE,
	PT_GLYPH_SCALE,

	// Colour
	PT_TEXT_R,
	PT_TEXT_G,
	PT_TEXT_B,
	PT_TEXT_OPACITY,
	PT_HEAD_R,
	PT_HEAD_G,
	PT_HEAD_B,
	PT_HEAD_BOOST,
	PT_BACK_R,
	PT_BACK_G,
	PT_BACK_B,
	PT_BACK_OPACITY,
	PT_GLOW,

	// Output. Declared by both plugins so that a composition can be moved
	// between them without the parameter list shifting underneath it; the
	// source plugin simply has nothing to mix against and ignores it.
	PT_MIX,

	PT_COUNT
};

/// Columns across the frame. 8..240, exponential.
int ColumnsFromParam( float value );

/// Rows, from the column count and the frame's aspect. Not a parameter: cells
/// are kept square, because a grid of oblong cells makes every glyph in it
/// oblong too, and the glyphs are the whole picture.
int RowsForAspect( int columns, int width, int height );

/// Rows per second. 0.5..45, exponential -- the slow end is where the useful
/// resolution is.
float SpeedFromParam( float value );

/// Trail length as a fraction of the run. 0.04..1.
float TrailFromParam( float value );

/// Glyph changes per second. 0..30, with a dead zone at the bottom so that
/// "no mutation" is reachable by dragging to zero rather than by luck.
float MutateFromParam( float value );

/// Brightness exponent along the trail. 0.4..4, 1 at the centre of the slider.
float FalloffFromParam( float value );

/// The seed. 1..9999 -- an integer, so that nudging the slider gives a
/// different rain rather than an imperceptibly different one.
uint32_t SeedFromParam( float value );

/// Glyph size within its cell. 0.25..1.6, 1 at three quarters of the travel so
/// that "fills the cell" is a place you can find without reading the manual.
float GlyphScaleFromParam( float value );

/// Head colour boost. 0..1 straight through, named for symmetry with the rest.
float HeadBoostFromParam( float value );

/// Glow. 0..2.
float GlowFromParam( float value );

} // namespace downpour
