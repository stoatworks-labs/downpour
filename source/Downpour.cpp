#include "Downpour.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "Diag.h"
#include "Glyphs.h"
#include "Shaders.h"

namespace downpour
{
namespace
{
/// The stream texture is laid out as rows of this many texels. Wide enough that
/// a million-codepoint document is under a thousand rows, narrow enough to stay
/// well inside GL_MAX_TEXTURE_SIZE on anything that runs Resolume.
constexpr int kStreamWidth = 1024;

int OptionFromParam( float value, int count )
{
	const int index = static_cast< int >( std::lround( value ) );
	return std::max( 0, std::min( count - 1, index ) );
}
} // namespace

DownpourPlugin::DownpourPlugin( bool overInput_ ) :
	overInput( overInput_ )
{
	diag::init();

	// A source takes no input; an effect takes exactly one. Getting this wrong
	// is not a compile error -- Resolume simply files the plugin under the
	// wrong tab and hands it a texture it was not expecting.
	SetMinInputs( overInput ? 1 : 0 );
	SetMaxInputs( overInput ? 1 : 0 );

	//---------------------------------------------------------------------
	// Defaults.
	//
	// They add up to green katakana on black at sixty columns, because a
	// generator that needs four sliders found before it looks like anything is
	// a generator nobody keeps. Every one of these is read back out through
	// GetFloatParameter, so these assignments are what the host is told.
	//---------------------------------------------------------------------
	params[ PT_SPEED ]     = 0.52f;// ~8 rows/sec
	params[ PT_DIRECTION ] = static_cast< float >( Direction::Down );
	params[ PT_COLUMNS ]   = 0.59f;// ~60 columns
	params[ PT_TRAIL ]     = 0.43f;
	params[ PT_DENSITY ]   = 0.85f;
	params[ PT_MUTATE ]    = 0.55f;
	params[ PT_FALLOFF ]   = 0.62f;
	params[ PT_SEED ]      = 0.0f;

	params[ PT_SOURCE ]  = static_cast< float >( Source::Junk );
	params[ PT_CHARSET ] = static_cast< float >( CharSet::KatakanaDigits );
	params[ PT_MIRROR ]  = 1.0f;// the film's glyphs are mirrored

	params[ PT_FONT ]        = 0.0f;
	params[ PT_GLYPH_SCALE ] = 0.75f;// 1.0, see GlyphScaleFromParam

	params[ PT_TEXT_R ]       = 0.30f;
	params[ PT_TEXT_G ]       = 1.00f;
	params[ PT_TEXT_B ]       = 0.42f;
	params[ PT_TEXT_OPACITY ] = 1.0f;
	params[ PT_HEAD_R ]       = 0.82f;
	params[ PT_HEAD_G ]       = 1.00f;
	params[ PT_HEAD_B ]       = 0.88f;
	params[ PT_HEAD_BOOST ]   = 1.0f;
	params[ PT_BACK_R ]       = 0.0f;
	params[ PT_BACK_G ]       = 0.04f;
	params[ PT_BACK_B ]       = 0.01f;
	params[ PT_BACK_OPACITY ] = 1.0f;
	params[ PT_GLOW ]         = 0.25f;

	params[ PT_MIX ] = 1.0f;

	customText = "Wake up...";

	//---------------------------------------------------------------------
	// Declaration.
	//
	// Every numeric parameter is a plain 0..1 float even where it stands for a
	// column count or a rate. SetParamInfo clamps an FF_TYPE_STANDARD default
	// into 0..1 *before* a range can be attached, so a parameter declared in
	// rows per second cannot declare a default in rows per second. The
	// conversions live in Controls.cpp.
	//---------------------------------------------------------------------
	SetParamInfof( PT_SPEED, "Speed", FF_TYPE_STANDARD );

	SetOptionParamInfo( PT_DIRECTION, "Direction", static_cast< int >( Direction::Count ), params[ PT_DIRECTION ] );
	SetParamElementInfo( PT_DIRECTION, 0, "Down", 0.0f );
	SetParamElementInfo( PT_DIRECTION, 1, "Up", 1.0f );
	SetParamElementInfo( PT_DIRECTION, 2, "Right", 2.0f );
	SetParamElementInfo( PT_DIRECTION, 3, "Left", 3.0f );

	SetParamInfof( PT_COLUMNS, "Columns", FF_TYPE_STANDARD );
	SetParamInfof( PT_TRAIL, "Trail", FF_TYPE_STANDARD );
	SetParamInfof( PT_DENSITY, "Density", FF_TYPE_STANDARD );
	SetParamInfof( PT_MUTATE, "Mutation", FF_TYPE_STANDARD );
	SetParamInfof( PT_FALLOFF, "Fade", FF_TYPE_STANDARD );
	SetParamInfof( PT_SEED, "Seed", FF_TYPE_STANDARD );

	SetOptionParamInfo( PT_SOURCE, "Source", static_cast< int >( Source::Count ), params[ PT_SOURCE ] );
	for( int i = 0; i < static_cast< int >( Source::Count ); ++i )
		SetParamElementInfo( PT_SOURCE, i, SourceName( static_cast< Source >( i ) ), static_cast< float >( i ) );

	SetOptionParamInfo( PT_CHARSET, "Characters", static_cast< int >( CharSet::Count ), params[ PT_CHARSET ] );
	for( int i = 0; i < static_cast< int >( CharSet::Count ); ++i )
		SetParamElementInfo( PT_CHARSET, i, CharSetName( static_cast< CharSet >( i ) ), static_cast< float >( i ) );

	// Left visible in every mode on purpose. A field that appears and
	// disappears as a neighbouring dropdown moves reads as a glitch, and this
	// way what you typed is still there when you come back to it.
	SetParamInfo( PT_CUSTOM_TEXT, "Custom Text", FF_TYPE_TEXT, customText.c_str() );
	SetFileParamInfo( PT_TEXT_FILE, "Text File", { "txt" }, "" );
	SetParamInfof( PT_MIRROR, "Mirror Glyphs", FF_TYPE_BOOLEAN );

	//---------------------------------------------------------------------
	// The font list is scanned here, once per instance. It has to be declared
	// in the constructor -- SetParamElementInfo is how a host learns a
	// dropdown's contents, and dynamic elements need Resolume 7.4.1, which is
	// a floor this plugin does not otherwise have.
	//---------------------------------------------------------------------
	const std::vector< FontFile >& fonts = InstalledFonts();
	const int fontCount = std::max( 1, static_cast< int >( fonts.size() ) + 1 );

	SetOptionParamInfo( PT_FONT, "Font", fontCount, params[ PT_FONT ] );
	SetParamElementInfo( PT_FONT, 0, "Built-in", 0.0f );
	for( size_t i = 0; i < fonts.size(); ++i )
		SetParamElementInfo( PT_FONT, static_cast< unsigned int >( i + 1 ), fonts[ i ].family.c_str(),
		                     static_cast< float >( i + 1 ) );

	SetFileParamInfo( PT_FONT_FILE, "Font File", { "ttf", "otf", "ttc", "otc" }, "" );
	SetParamInfof( PT_GLYPH_SCALE, "Glyph Size", FF_TYPE_STANDARD );

	// Consecutive red/green/blue parameters are what a host needs to show a
	// colour swatch instead of three sliders, so the naming follows the SDK's
	// own convention rather than being tidied up.
	SetParamInfof( PT_TEXT_R, "Text", FF_TYPE_RED );
	SetParamInfof( PT_TEXT_G, "Text_Green", FF_TYPE_GREEN );
	SetParamInfof( PT_TEXT_B, "Text_Blue", FF_TYPE_BLUE );
	SetParamInfof( PT_TEXT_OPACITY, "Text Opacity", FF_TYPE_STANDARD );

	SetParamInfof( PT_HEAD_R, "Head", FF_TYPE_RED );
	SetParamInfof( PT_HEAD_G, "Head_Green", FF_TYPE_GREEN );
	SetParamInfof( PT_HEAD_B, "Head_Blue", FF_TYPE_BLUE );
	SetParamInfof( PT_HEAD_BOOST, "Head Boost", FF_TYPE_STANDARD );

	SetParamInfof( PT_BACK_R, "Background", FF_TYPE_RED );
	SetParamInfof( PT_BACK_G, "Background_Green", FF_TYPE_GREEN );
	SetParamInfof( PT_BACK_B, "Background_Blue", FF_TYPE_BLUE );
	SetParamInfof( PT_BACK_OPACITY, "Background Opacity", FF_TYPE_STANDARD );

	SetParamInfof( PT_GLOW, "Glow", FF_TYPE_STANDARD );
	SetParamInfof( PT_MIX, "Mix", FF_TYPE_STANDARD );

	// Thirty parameters is well past the point where an ungrouped list in
	// somebody else's inspector stops being readable. SetParamGroup collapses
	// *runs* of same-group parameters, which is why the ids in Controls.h have
	// to stay in this order.
	for( unsigned int id = PT_SPEED; id <= PT_SEED; ++id )
		SetParamGroup( id, "Rain" );
	for( unsigned int id = PT_SOURCE; id <= PT_MIRROR; ++id )
		SetParamGroup( id, "Text" );
	for( unsigned int id = PT_FONT; id <= PT_GLYPH_SCALE; ++id )
		SetParamGroup( id, "Font" );
	for( unsigned int id = PT_TEXT_R; id <= PT_GLOW; ++id )
		SetParamGroup( id, "Colour" );
	SetParamGroup( PT_MIX, "Output" );
}

//---------------------------------------------------------------------------
// Parameters
//---------------------------------------------------------------------------
FFResult DownpourPlugin::SetFloatParameter( unsigned int index, float value )
{
	if( index >= PT_COUNT )
		return FF_FAIL;

	const float previous = params[ index ];
	params[ index ]      = value;

	if( index == PT_SOURCE || index == PT_CHARSET )
		contentDirty = true;

	if( index == PT_FONT && value != previous )
	{
		// The operator moved the dropdown, so the index wins over the family
		// name remembered from the composition -- once. See the note on
		// fontIndexChosen in the header.
		fontIndexChosen = true;
		contentDirty    = true;
	}

	return FF_SUCCESS;
}

float DownpourPlugin::GetFloatParameter( unsigned int index )
{
	return index < PT_COUNT ? params[ index ] : 0.0f;
}

FFResult DownpourPlugin::SetTextParameter( unsigned int index, const char* value )
{
	const std::string incoming = value != nullptr ? value : "";

	std::lock_guard< std::mutex > lock( textMutex );
	switch( index )
	{
	case PT_CUSTOM_TEXT:
		customText   = incoming;
		contentDirty = true;
		return FF_SUCCESS;

	case PT_TEXT_FILE:
		textFilePath = incoming;
		contentDirty = true;
		return FF_SUCCESS;

	case PT_FONT_FILE:
		fontFilePath = incoming;
		contentDirty = true;
		return FF_SUCCESS;

	default:
		return FF_FAIL;
	}
}

char* DownpourPlugin::GetTextParameter( unsigned int index )
{
	std::lock_guard< std::mutex > lock( textMutex );

	const std::string* source = nullptr;
	switch( index )
	{
	case PT_CUSTOM_TEXT: source = &customText;   break;
	case PT_TEXT_FILE:   source = &textFilePath; break;
	case PT_FONT_FILE:   source = &fontFilePath; break;
	default:             break;
	}

	textReturn[ 0 ] = '\0';
	if( source != nullptr )
	{
		const size_t length = std::min( source->size(), sizeof( textReturn ) - 1 );
		std::memcpy( textReturn, source->data(), length );
		textReturn[ length ] = '\0';
	}
	return textReturn;
}

//---------------------------------------------------------------------------
// Content
//---------------------------------------------------------------------------
void DownpourPlugin::ResolveTypeface()
{
	std::string wantedFile;
	std::string wantedFamily;
	{
		std::lock_guard< std::mutex > lock( textMutex );
		wantedFile   = fontFilePath;
		wantedFamily = fontFamily;
	}

	// An explicit file beats everything. It is the escape hatch for a font that
	// is not installed, and someone who picked one meant it.
	if( !wantedFile.empty() && typeface.Load( wantedFile, 0 ) )
	{
		std::lock_guard< std::mutex > lock( textMutex );
		fontFamily = typeface.Family();
		return;
	}

	const std::vector< FontFile >& fonts = InstalledFonts();
	const int index = OptionFromParam( params[ PT_FONT ], static_cast< int >( fonts.size() ) + 1 );

	// Index 0 is the built-in face, which is also where an unresolvable choice
	// lands rather than leaving the frame blank.
	int chosen = -1;

	if( fontIndexChosen )
	{
		chosen          = index - 1;
		fontIndexChosen = false;
	}
	else if( !wantedFamily.empty() )
	{
		// Restored from a composition. The name is authoritative because the
		// index means whatever the *saving* machine had installed at that
		// position, which is very unlikely to be this machine's.
		chosen = FindFontByFamily( wantedFamily );
		if( chosen < 0 )
			diag::warn( "font '" + wantedFamily + "' is not installed, using the built-in face" );
	}
	else
	{
		chosen = index - 1;
	}

	if( chosen >= 0 && chosen < static_cast< int >( fonts.size() )
	    && typeface.Load( fonts[ chosen ].path, fonts[ chosen ].collectionIndex ) )
	{
		std::lock_guard< std::mutex > lock( textMutex );
		fontFamily = fonts[ chosen ].family;
		return;
	}

	typeface.UseBuiltin();
	std::lock_guard< std::mutex > lock( textMutex );
	fontFamily.clear();
}

void DownpourPlugin::RebuildContent()
{
	std::string text;
	std::string file;
	{
		std::lock_guard< std::mutex > lock( textMutex );
		text = customText;
		file = textFilePath;
	}

	const Source source = static_cast< Source >( OptionFromParam( params[ PT_SOURCE ], static_cast< int >( Source::Count ) ) );
	const CharSet set   = static_cast< CharSet >( OptionFromParam( params[ PT_CHARSET ], static_cast< int >( CharSet::Count ) ) );

	stream = BuildStream( source, set, text, file );

	ResolveTypeface();

	int fromFont = 0, fromBuiltin = 0, missing = 0;
	typeface.BuildAtlas( stream.codepoints, atlasImage, slotForCodepoint, fromFont, fromBuiltin, missing );
	atlasCounts = AtlasCounts { fromFont, fromBuiltin, missing };

	// The stream texture holds atlas slots, not code points. Doing the mapping
	// here rather than in the shader is what lets the shader stay one texelFetch
	// away from a glyph.
	streamSlots.clear();
	streamSlots.reserve( stream.codepoints.size() );
	for( uint32_t codepoint : stream.codepoints )
	{
		const auto found = slotForCodepoint.find( codepoint );
		streamSlots.push_back( static_cast< float >( found == slotForCodepoint.end() ? kBlankSlot : found->second ) );
	}

	if( streamSlots.empty() )
		streamSlots.push_back( static_cast< float >( kBlankSlot ) );

	streamWidth  = std::min( kStreamWidth, static_cast< int >( streamSlots.size() ) );
	streamHeight = ( static_cast< int >( streamSlots.size() ) + streamWidth - 1 ) / streamWidth;
	streamSlots.resize( static_cast< size_t >( streamWidth ) * streamHeight,
	                    static_cast< float >( kBlankSlot ) );

	diag::info( "content rebuilt: " + stream.note + ", " + std::to_string( stream.codepoints.size() )
	            + " codepoints, glyphs " + std::to_string( fromFont ) + " from font / "
	            + std::to_string( fromBuiltin ) + " built-in / " + std::to_string( missing ) + " missing" );

	contentDirty = false;
	uploadDirty  = true;
}

std::string DownpourPlugin::SourceNote() const
{
	std::lock_guard< std::mutex > lock( textMutex );
	std::string note = stream.note;
	if( !fontFamily.empty() )
		note += ", font " + fontFamily;
	else
		note += ", built-in face";
	return note;
}

int DownpourPlugin::SlotForCodepoint( uint32_t codepoint ) const
{
	const auto found = slotForCodepoint.find( codepoint );
	return found == slotForCodepoint.end() ? -1 : found->second;
}

RainState DownpourPlugin::CurrentState( int width, int height ) const
{
	RainState state;
	state.time      = static_cast< float >( hostTime );
	state.columns   = ColumnsFromParam( params[ PT_COLUMNS ] );
	state.rows      = RowsForAspect( state.columns, width, height );
	state.speed     = SpeedFromParam( params[ PT_SPEED ] );
	state.trail     = TrailFromParam( params[ PT_TRAIL ] );
	state.density   = params[ PT_DENSITY ];
	state.mutate    = MutateFromParam( params[ PT_MUTATE ] );
	state.falloff   = FalloffFromParam( params[ PT_FALLOFF ] );
	state.seed      = SeedFromParam( params[ PT_SEED ] );
	state.direction = static_cast< Direction >( OptionFromParam( params[ PT_DIRECTION ], static_cast< int >( Direction::Count ) ) );
	state.flow      = stream.flow;
	state.streamLength = std::max( 1, static_cast< int >( stream.codepoints.size() ) );
	return state;
}

//---------------------------------------------------------------------------
// GL
//---------------------------------------------------------------------------
bool DownpourPlugin::UploadAtlas()
{
	if( atlasImage.size() < static_cast< size_t >( kAtlasPx ) * kAtlasPx )
		return false;

	if( atlasTexture == 0 )
		glGenTextures( 1, &atlasTexture );

	glBindTexture( GL_TEXTURE_2D, atlasTexture );
	glPixelStorei( GL_UNPACK_ALIGNMENT, 1 );
	glTexImage2D( GL_TEXTURE_2D, 0, GL_R8, kAtlasPx, kAtlasPx, 0, GL_RED, GL_UNSIGNED_BYTE,
	              atlasImage.data() );

	// Mipmapped and linear. A cell can be four pixels across or four hundred
	// depending on the column count and the output size, and a hardware box
	// filter gets the average right at any ratio for one fetch.
	glGenerateMipmap( GL_TEXTURE_2D );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	glBindTexture( GL_TEXTURE_2D, 0 );

	return true;
}

bool DownpourPlugin::UploadStream()
{
	if( streamSlots.empty() || streamWidth <= 0 || streamHeight <= 0 )
		return false;

	if( streamTexture == 0 )
		glGenTextures( 1, &streamTexture );

	glBindTexture( GL_TEXTURE_2D, streamTexture );
	glPixelStorei( GL_UNPACK_ALIGNMENT, 1 );
	glTexImage2D( GL_TEXTURE_2D, 0, GL_R32F, streamWidth, streamHeight, 0, GL_RED, GL_FLOAT,
	              streamSlots.data() );

	// GL_NEAREST and no mipmaps, and it is read with texelFetch anyway. These
	// values are slot numbers, not quantities: interpolate two of them and you
	// address a third glyph that neither cell asked for.
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	glBindTexture( GL_TEXTURE_2D, 0 );

	return true;
}

FFResult DownpourPlugin::InitGL( const FFGLViewportStruct* vp )
{
	diag::init();
	diag::info( std::string( "InitGL, GL " )
	            + reinterpret_cast< const char* >( glGetString( GL_VERSION ) != nullptr ? glGetString( GL_VERSION ) : reinterpret_cast< const GLubyte* >( "unknown" ) ) );

	// One shader, compiled with or without the input path. A shader that will
	// not compile is the failure that actually happens, and from the operator's
	// side it looks like "the plugin does nothing" with no message anywhere --
	// which is what Diag is for.
	std::string fragment = kRainShader;
	{
		// After the #version line, which must be first in a GLSL source.
		const size_t afterVersion = fragment.find( '\n' );
		if( afterVersion != std::string::npos )
		{
			std::string defines;
			if( overInput )
				defines += kEffectDefine;
			if( cellDebug )
				defines += kCellDebugDefine;
			fragment.insert( afterVersion + 1, defines );
		}
	}

	if( !shader.Compile( kVertexShader, fragment.c_str() ) )
	{
		diag::error( "rain shader would not compile" );
		DeInitGL();
		return FF_FAIL;
	}

	if( !quad.Initialise() )
	{
		diag::error( "screen quad would not initialise" );
		DeInitGL();
		return FF_FAIL;
	}

	contentDirty = true;
	return CFFGLPlugin::InitGL( vp );
}

void DownpourPlugin::Render( int width, int height, GLuint inputTexture, float maxU, float maxV )
{
	if( contentDirty )
		RebuildContent();

	if( uploadDirty )
	{
		UploadAtlas();
		UploadStream();
		uploadDirty = false;
	}

	const RainState state = CurrentState( width, height );

	ffglex::ScopedShaderBinding shaderBinding( shader.GetGLID() );

	glActiveTexture( GL_TEXTURE0 );
	glBindTexture( GL_TEXTURE_2D, atlasTexture );
	glActiveTexture( GL_TEXTURE1 );
	glBindTexture( GL_TEXTURE_2D, streamTexture );

	shader.Set( "AtlasTexture", 0 );
	shader.Set( "StreamTexture", 1 );

	shader.Set( "Time", state.time );
	shader.Set( "Grid", static_cast< float >( state.columns ), static_cast< float >( state.rows ) );
	shader.Set( "Speed", state.speed );
	shader.Set( "Trail", state.trail );
	shader.Set( "Density", state.density );
	shader.Set( "Mutate", state.mutate );
	shader.Set( "Falloff", state.falloff );
	shader.Set( "DirectionMode", static_cast< int >( state.direction ) );
	shader.Set( "FlowMode", static_cast< int >( state.flow ) );
	shader.Set( "StreamLength", state.streamLength );
	shader.Set( "StreamSize", static_cast< float >( streamWidth ), static_cast< float >( streamHeight ) );
	shader.Set( "Resolution", static_cast< float >( width ), static_cast< float >( height ) );

	// Seed is a uint in the shader; FFGLShader has no uint setter, so it goes
	// through the raw uniform. A mismatch here is silent -- glUniform on -1 is
	// a documented no-op -- which is what tools/sweep.py exists to catch.
	glUniform1ui( glGetUniformLocation( shader.GetGLID(), "Seed" ), state.seed );

	shader.Set( "AtlasLayout", static_cast< float >( kAtlasCols ), static_cast< float >( kAtlasRows ) );
	shader.Set( "AtlasSize", static_cast< float >( kAtlasPx ), static_cast< float >( kAtlasPx ) );
	shader.Set( "AtlasMetrics", static_cast< float >( kSlotPx ), static_cast< float >( kGlyphPx ),
	            static_cast< float >( kBorderPx ) );
	shader.Set( "GlyphScale", GlyphScaleFromParam( params[ PT_GLYPH_SCALE ] ) );
	shader.Set( "MirrorGlyphs", params[ PT_MIRROR ] >= 0.5f ? 1.0f : 0.0f );

	shader.Set( "TextColour", params[ PT_TEXT_R ], params[ PT_TEXT_G ], params[ PT_TEXT_B ] );
	shader.Set( "TextOpacity", params[ PT_TEXT_OPACITY ] );
	shader.Set( "HeadColour", params[ PT_HEAD_R ], params[ PT_HEAD_G ], params[ PT_HEAD_B ] );
	shader.Set( "HeadBoost", HeadBoostFromParam( params[ PT_HEAD_BOOST ] ) );
	shader.Set( "BackColour", params[ PT_BACK_R ], params[ PT_BACK_G ], params[ PT_BACK_B ] );
	shader.Set( "BackOpacity", params[ PT_BACK_OPACITY ] );
	shader.Set( "Glow", GlowFromParam( params[ PT_GLOW ] ) );

	if( overInput )
	{
		glActiveTexture( GL_TEXTURE2 );
		glBindTexture( GL_TEXTURE_2D, inputTexture );
		shader.Set( "InputTexture", 2 );
		shader.Set( "MaxUV", maxU, maxV );
		shader.Set( "Mix_", params[ PT_MIX ] );
	}

	quad.Draw();

	glActiveTexture( GL_TEXTURE2 );
	glBindTexture( GL_TEXTURE_2D, 0 );
	glActiveTexture( GL_TEXTURE1 );
	glBindTexture( GL_TEXTURE_2D, 0 );
	glActiveTexture( GL_TEXTURE0 );
	glBindTexture( GL_TEXTURE_2D, 0 );
}

FFResult DownpourPlugin::ProcessOpenGL( ProcessOpenGLStruct* pGL )
{
	int width  = 0;
	int height = 0;
	GLuint inputTexture = 0;
	float maxU = 1.0f;
	float maxV = 1.0f;

	if( overInput )
	{
		if( pGL == nullptr || pGL->numInputTextures < 1 || pGL->inputTextures[ 0 ] == nullptr )
			return FF_FAIL;

		const FFGLTextureStruct& texture = *pGL->inputTextures[ 0 ];
		inputTexture = texture.Handle;
		width        = texture.Width;
		height       = texture.Height;

		// The input texture can be bigger than the picture; MaxUV is the
		// fraction that was really drawn. The rain itself is drawn in frame
		// space and never touches this -- only the fetch of the clip does.
		const FFGLTexCoords coords = GetMaxGLTexCoords( texture );
		maxU = coords.s;
		maxV = coords.t;
	}
	else
	{
		width  = static_cast< int >( currentViewport.width );
		height = static_cast< int >( currentViewport.height );
	}

	if( width <= 0 || height <= 0 )
		return FF_FAIL;

	Render( width, height, inputTexture, maxU, maxV );
	return FF_SUCCESS;
}

FFResult DownpourPlugin::DeInitGL()
{
	shader.FreeGLResources();
	quad.Release();

	if( atlasTexture != 0 )
	{
		glDeleteTextures( 1, &atlasTexture );
		atlasTexture = 0;
	}
	if( streamTexture != 0 )
	{
		glDeleteTextures( 1, &streamTexture );
		streamTexture = 0;
	}

	uploadDirty = true;
	return FF_SUCCESS;
}

} // namespace downpour
