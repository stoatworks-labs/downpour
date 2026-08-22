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

	// Preset. Declared after the real controls so their ids — which a saved
	// composition refers to — do not shift under existing users.
	PT_PRESET,

	// Sync. Appended for the same reason: it arrived after v0.1.0 shipped, and
	// inserting it next to Speed — where it belongs — would renumber every id
	// after it in every saved composition.
	PT_SYNC,

	// Audio. Appended likewise. PT_AUDIO is an FFT buffer (FF_TYPE_BUFFER,
	// FF_USAGE_FFT): Resolume shows it as an audio-source picker and writes
	// one spectrum bin per element, low frequencies first. FFGL only; OFX
	// hosts have no audio analysis and never see these.
	PT_AUDIO,
	PT_AUDIO_LEVEL,

	// -- The Stoatworks About block ------------------------------------------
	//
	// One display-only text line, then one button per link the block carries:
	// the guide, the project page, the source, the funding page. A button opens
	// a browser and stores nothing.
	//
	// How many buttons there are is decided by which URLs StoatworksAbout.h
	// actually holds, so Downpour.cpp static_asserts this run against
	// `about::kParamCount` -- writing a user guide later adds one, and without
	// the assert that would silently shift PT_COUNT and leave the last button
	// undeclared.
	//
	// Last in the enum so no saved composition's parameter ids shift.
	PT_ABOUT_TEXT,
	PT_ABOUT_BUTTON_1,
	PT_ABOUT_BUTTON_2,
	PT_ABOUT_BUTTON_3,
	PT_COUNT
};

/// Spectrum bins in the Audio buffer parameter, in RainState::audio, and in
/// the shader's `Audio[]` uniform. All three must agree, and the shader's is
/// a literal.
constexpr int kAudioBins = 64;

/// What Speed is measured against. Free: per second, on the host's clock.
/// Beat and Bar: per beat or per bar, locked to the host's BPM clock, with
/// each interval's travel eased in hard at the front so the rain visibly
/// kicks on the grid. Mutation rides the same clock scaled by Speed — see
/// kMutateReferenceSpeed.
enum class SyncMode : int
{
	Free = 0,
	Beat,
	Bar,

	Count
};

/// Columns across the frame. 8..240, exponential.
int ColumnsFromParam( float value );

/// Rows, from the column count and the frame's aspect. Not a parameter: cells
/// are kept square, because a grid of oblong cells makes every glyph in it
/// oblong too, and the glyphs are the whole picture.
int RowsForAspect( int columns, int width, int height );

/// Rows per second -- or per beat or per bar under those Sync modes.
/// 0.5..45, exponential -- the slow end is where the useful resolution is.
float SpeedFromParam( float value );

/// Trail length as a fraction of the run. 0.04..1.
float TrailFromParam( float value );

/// Glyph changes per second. 0..30, with a dead zone at the bottom so that
/// "no mutation" is reachable by dragging to zero rather than by luck.
///
/// Calibrated at the default Speed: callers scale the returned rate by
/// speed / kMutateReferenceSpeed, so churn slows and quickens with the rain
/// itself. Winding Speed to the bottom used to leave every lit cell
/// re-rolling ~4 times a second — 13x the pixel change of the crawling heads
/// — so the rain read as a storm at any Speed (issue #1). At the default
/// Speed the scale is 1 and the look is exactly v0.2.0's.
float MutateFromParam( float value );

/// The Speed at which MutateFromParam's per-second calibration holds:
/// SpeedFromParam(0.52), the default. Divide the current speed by this to
/// scale a mutation rate.
constexpr float kMutateReferenceSpeed = 5.19f;

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
