#pragma once

#include <FFGLSDK.h>

#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "Controls.h"
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

	float params[ PT_COUNT ] = { 0.0f };
};

} // namespace downpour
