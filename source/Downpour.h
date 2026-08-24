#pragma once

#include <FFGLSDK.h>

// After FFGLSDK.h, which is where FFUInt32 comes from.
#include "StoatworksAboutParams.h"

#include <array>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "Controls.h"
#include "Presets.h"
#include "Rain.h"
#include "TextSource.h"
#include "Typeface.h"

/**
    Downpour -- a digital rain generator for Resolume.

    Falling columns of characters. What is worth knowing about how it works is
    in three other files and not repeated here:

    - **Rain.h** -- a cell is a pure function of (column, row, time). No
      simulation state, so nothing drifts with the frame rate and any frame can
      be rendered on its own.
    - **Shaders.h** -- one pass, and why there is no cell buffer.
    - **Typeface.h** -- how a font is found, and the per-codepoint fallback that
      makes "pick any font" safe to offer.

    This class is the part that talks to the host: it declares the parameters,
    keeps the stream and the atlas up to date when they change, and draws.

    Both plugins are this class. The source draws rain over its own background;
    the effect draws the same rain over the incoming clip. They differ by a
    constructor flag, a `#define` handed to the shader compiler, and their input
    count -- which is little enough that keeping them as one class is what stops
    them drifting apart, and much of what a second implementation would have got
    wrong is in the colour compositing they now share.

    See AGENTS.md for the traps.
*/
namespace downpour
{
class DownpourPlugin : public CFFGLPlugin
{
public:
	explicit DownpourPlugin( bool overInput );

	// CFFGLPlugin
	FFResult InitGL( const FFGLViewportStruct* vp ) override;
	FFResult ProcessOpenGL( ProcessOpenGLStruct* pGL ) override;
	FFResult DeInitGL() override;

	FFResult SetFloatParameter( unsigned int index, float value ) override;
	float GetFloatParameter( unsigned int index ) override;

	/// Test hook: the parameter ids a preset covers, in presets::Param
	/// order. Handed out rather than copied into the harness, so a second
	/// list cannot go quietly out of step with this one.
	static const unsigned int* PresetParamIDsForTest( int& count );

	FFResult SetTextParameter( unsigned int index, const char* value ) override;
	char* GetTextParameter( unsigned int index ) override;

	/// Render one frame into whatever is currently bound, at `width` x `height`.
	/// Exposed for the offline harness, which drives this class rather than a
	/// copy of it -- a test that exercises a reimplementation tests the
	/// reimplementation.
	void Render( int width, int height, GLuint inputTexture, float maxU, float maxV );

	/// What the rain would be at this size and time, for the harness to check
	/// the rendered picture against.
	RainState CurrentState( int width, int height ) const;

	/// Atlas slot for a codepoint under the current font, or -1 if the current
	/// stream never asked for it.
	int SlotForCodepoint( uint32_t codepoint ) const;

	/// The stream as resolved, for `--readback` to compare against.
	const std::vector< uint32_t >& StreamCodepoints() const { return stream.codepoints; }

	/// The atlas image as last built, for `--atlas`.
	const std::vector< uint8_t >& AtlasImage() const { return atlasImage; }

	/// One line describing where the characters came from and what fell back.
	std::string SourceNote() const;

	/// How the last atlas build was sourced. The `builtin` count is what proves
	/// the per-codepoint fallback is working: pick a font with no katakana and
	/// this is where the katakana come from.
	struct AtlasCounts
	{
		int fromFont    = 0;
		int fromBuiltin = 0;
		int missing     = 0;
	};
	AtlasCounts LastAtlasCounts() const { return atlasCounts; }

	/// Force the stream and atlas to be rebuilt before the next draw. The
	/// harness needs this because it sets parameters directly.
	void Invalidate() { contentDirty = true; }

	/// Compile the shader so it writes raw cell state instead of a picture, for
	/// `dptest --rain`. Must be called before InitGL; the define is prepended at
	/// compile time, so a shipping plugin never carries the branch.
	void EnableCellDebug() { cellDebug = true; }

private:
	/// The ParamId each presets::Param drives, in presets::Param order. The
	/// preset table stays host-agnostic; this is the FFGL binding of it.
	static constexpr unsigned int kPresetParamIDs[ presets::kParamCount ] = {
		PT_SPEED, PT_DIRECTION, PT_COLUMNS, PT_TRAIL, PT_DENSITY, PT_MUTATE, PT_FALLOFF,
		PT_SOURCE, PT_CHARSET, PT_MIRROR, PT_GLYPH_SCALE,
		PT_TEXT_R, PT_TEXT_G, PT_TEXT_B, PT_TEXT_OPACITY,
		PT_HEAD_R, PT_HEAD_G, PT_HEAD_B, PT_HEAD_BOOST,
		PT_BACK_R, PT_BACK_G, PT_BACK_B, PT_BACK_OPACITY, PT_GLOW
	};

	/// Copy a factory preset's values into params[] and raise value events so
	/// the host re-reads the sliders. `presetIndex` is 1-based; 0 is Custom.
	/// The active preset's value for `id`, or -1 when no preset is active or
	/// this one has no opinion about `id`. Preset values are all 0..1, so a
	/// negative is unambiguous.
	float presetValue( int presetIndex, unsigned int id ) const;

	/// True when this write is the HOST restating a value it still believes in
	/// rather than the operator moving anything -- in which case it must not
	/// reach params[] and must not disturb the preset.
	bool hostIsRestatingItself( unsigned int index, float value );

	/// Record the defaults as the host's opening position, once, before
	/// anything has had a chance to move them.
	void seedHostValues();

	void applyPreset( int presetIndex );

	/// What the HOST last sent for each parameter, which is not the same thing
	/// as what the plugin is rendering with.
	///
	/// FFGL's host owns parameter state. It pushes its own values back down
	/// whenever it likes, and nothing obliges it to act on the value events
	/// applyPreset raises -- Resolume does not. So a preset that writes params[]
	/// and trusts the host to follow is relying on behaviour the specification
	/// never promised, and when the host instead restates the values it still
	/// believes in, the rule that a covered parameter changing means the
	/// operator has taken over fires on the host's own echo and drops straight
	/// back to Custom. Reported against vertigo as its issue #2; the same
	/// pattern had been copied into all seven plugins.
	///
	/// Keeping the host's own last word separately is what tells the two apart.
	float hostValues[ PT_COUNT ] = {};
	bool hostValuesSeeded        = false;

	void RebuildContent();
	bool UploadAtlas();
	bool UploadStream();
	void ResolveTypeface();

	const bool overInput;
	bool cellDebug = false;

	ffglex::FFGLShader shader;
	ffglex::FFGLScreenQuad quad;

	GLuint atlasTexture  = 0;
	GLuint streamTexture = 0;

	//---------------------------------------------------------------------
	// Content: the stream, the font, and the atlas built from the two.
	//
	// Rebuilt on a parameter change and then left alone. Never per frame --
	// scanning a font, rasterising a hundred glyphs and uploading four
	// megabytes is not a thing to do sixty times a second, and none of it
	// depends on the clock.
	//---------------------------------------------------------------------
	Typeface typeface;
	Stream stream;
	std::unordered_map< uint32_t, int > slotForCodepoint;
	std::vector< uint8_t > atlasImage;
	AtlasCounts atlasCounts;
	std::vector< float > streamSlots;///< atlas slot per stream position
	int streamWidth  = 0;
	int streamHeight = 0;

	bool contentDirty = true;
	bool uploadDirty  = true;

	//---------------------------------------------------------------------
	// Text parameters.
	//
	// The only plugin state that is not a float, and the only place the host's
	// thread and the render thread touch the same object rather than racing on
	// a word. A std::string reallocating under a reader is a crash in somebody
	// else's process, so it gets a lock -- taken on a change and on a rebuild,
	// never per frame.
	//---------------------------------------------------------------------
	mutable std::mutex textMutex;
	std::string customText;
	std::string textFilePath;
	std::string fontFilePath;

	/// The family name of whatever font is actually in use.
	///
	/// This exists because **a composition stores the dropdown's numeric value,
	/// and that number means a different font on a machine with a different set
	/// of fonts installed.** The host serialises whatever GetTextParameter
	/// returns, so returning the resolved family here puts a portable name in
	/// the composition file at no cost to the operator, and `ResolveTypeface`
	/// prefers it over the index when the two disagree.
	std::string fontFamily;

	/// Set when the operator moves the Font dropdown, so that the index wins
	/// over the remembered name exactly once -- otherwise the name restored
	/// from the composition would keep overriding every new choice.
	bool fontIndexChosen = false;

	char textReturn[ 4096 ] = { 0 };

	//---------------------------------------------------------------------
	// Host clock units.
	//
	// The FFGL header never says what unit SetTime is in, and hosts disagree:
	// Resolume hands over MILLISECONDS (measured live: 20.0 per frame at its
	// 50 fps, and the SDK's own Particles sample divides by 1000), while the
	// offline harness sends seconds. UpdateClock calibrates the host's clock
	// against a steady_clock and lets the ratio name the unit, over several
	// agreeing frames, failing safe to the wall clock while undecided.
	// `hostSeconds` is the normalised clock everything downstream reads.
	//---------------------------------------------------------------------
	void UpdateClock();

public:
	FFResult SetTime( double time ) override;

	//---------------------------------------------------------------------
	// Clock test hooks. The offline harness DECLARES its unit rather than
	// leaving UpdateClock to infer one -- a single absolute time handed over
	// in one frame is genuinely ambiguous, and an implicit unit is what let
	// the millisecond bug through in the first place.
	//---------------------------------------------------------------------
	void SetClockScaleForTest( double scale );
	void TickClockForTest();
	double ClockScaleForTest() const;
	double HostSecondsForTest() const;

	/// How far the rain has fallen, and how many glyph changes have gone by, at
	/// this moment. `--speed` needs them: the thing being tested is that a Speed
	/// change does NOT move the rain, and reading the position either side of
	/// one says so directly. Comparing rendered frames could not: the rain is a
	/// field of columns on a cycle, so a travel a whole number of cycles away
	/// renders identically and would match for entirely the wrong reason.
	float TravelForTest() const;
	float MutateTicksForTest() const;

private:

	double clockScale  = 0.0;///< 0 until decided; then 1.0 or 0.001
	double lastWallTime = -1.0;
	double wallStart    = -1.0;
	int secondsVotes    = 0;
	int millisVotes     = 0;
	bool hostTimeSeen   = false;
	double lastRawTime = -1.0;
	double hostSeconds = 0.0;

	//---------------------------------------------------------------------
	// Where the rain has got to.
	//
	// The picture stays a pure function of the RainState -- that is the whole
	// design and none of it changes here. What changes is only which travel a
	// given clock reading maps to.
	//
	// Travel was `time * speed`, so a Speed change moved the rain by
	// `time * delta`, and `time` is however long the composition has been open.
	// An hour in, a small nudge is worth hundreds of cycles: every column
	// teleports and every drop lands somewhere unrelated. That is what orrery
	// issue #6 reported for its Speed control, once the 1000x clock bug was out
	// of the way.
	//
	// Mutate needs its own anchor rather than sharing one, for two reasons: it
	// has its own control, and it is SCALED by Speed (see
	// kMutateReferenceSpeed), so a Speed change moves the glyph churn as well.
	// Anchoring travel alone would leave every cell re-rolling its glyph the
	// instant Speed was touched -- the rain would hold still and the characters
	// would convulse.
	//
	// Free only. Beat and Bar are recovered from the transport each frame and
	// deliberately re-lock, because landing a cycle on the bar line is the whole
	// point of them; the anchors follow the clock while they are selected so
	// that returning to Free resumes rather than leaps. Nor is any of this in
	// the OpenFX build, which renders arbitrary times in arbitrary order and can
	// keyframe Speed: a running carry there would make a frame depend on which
	// frames were rendered before it.
	//---------------------------------------------------------------------
	void UpdateRainAnchor();

	/// The clock the rain runs on: seconds in Free, a curved bar count otherwise.
	double RainSeconds() const;

	/// Whether the anchors apply -- Free only.
	bool RainIsFree() const;

	/// The one copy of each expression. CurrentState fills the RainState from
	/// them and the test hooks read them, so a test cannot end up measuring
	/// different arithmetic from the plugin.
	float TravelAt( float speed ) const;
	float MutateTicksAt( float mutate ) const;

	float travelAnchor  = 0.0f;///< rows travelled by `anchorClock`
	float mutateAnchor  = 0.0f;///< glyph changes elapsed by `anchorClock`
	float anchorClock   = 0.0f;///< the clock reading both belong to
	float anchorSpeed   = -1.0f;///< speed in force since then; < 0 until the first frame
	float anchorMutate  = -1.0f;///< and the mutate rate

	//---------------------------------------------------------------------
	// Audio.
	//
	// The host writes one spectrum bin per element of PT_AUDIO; UpdateAudio
	// runs them through an attack/release filter into `audioLevel`, and
	// CurrentState hands that to the rain as its per-column gate.
	//---------------------------------------------------------------------
	void UpdateAudio();

	std::array< float, kAudioBins > audioLevel = {};
	double audioClock = -1.0;

	float params[ PT_COUNT ] = { 0.0f };
};

} // namespace downpour
