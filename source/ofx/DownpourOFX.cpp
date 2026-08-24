/// The OpenFX builds of Downpour, for DaVinci Resolve, Nuke, Natron, Vegas
/// and other OFX hosts. Two plugins from this one file, exactly as the FFGL
/// side ships two bundles: "Downpour" is a generator, "Downpour Over" draws
/// the same rain over the incoming clip.
///
/// The rain itself lives once, in Rain.cpp, and this file calls it — the same
/// C++ that dptest measures the GLSL against. The stream, the atlas and the
/// typeface handling are the same CPU code the FFGL build uses. What is
/// mirrored here from Shaders.cpp is only the per-pixel machinery: glyph
/// fitting, atlas sampling and the composite. When editing that part of the
/// fragment shader, edit this too.
///
/// One OFX-specific point worth stating: OFX hands render time in *frames*,
/// and the rain wants seconds. The conversion uses the clip's frame rate, so
/// a frame rendered twice is identical and a frame rendered alone is exactly
/// what playback would have shown — the properties the whole plugin is built
/// around.

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "ofxsImageEffect.h"
#include "ofxsProcessing.h"

// After the OFX Support headers, which is where the OFX types come from.
#include "StoatworksAboutOFX.h"

#include "../Controls.h"
#include "../Glyphs.h"
#include "../Presets.h"
#include "../Rain.h"
#include "../TextSource.h"
#include "../Typeface.h"

namespace
{
constexpr const char* kSourceIdentifier = "com.stoatworks.downpour";
constexpr const char* kOverIdentifier   = "com.stoatworks.downpourover";
constexpr const char* kPluginGrouping   = "Stoatworks";
constexpr const char* kPluginDescription =
	"Digital rain: falling columns of characters.\n\n"
	"A cell is a pure function of (column, row, time) — no simulation state — "
	"so the rain cannot drift against a cue, any frame can be rendered on its "
	"own, and the picture is identical at every resolution. Sources range "
	"from the classic katakana junk to whole documents that play out in "
	"order.\n\n"
	"https://stoatworks-labs.com";

constexpr const char* kParamSpeed       = "speed";
constexpr const char* kParamDirection   = "direction";
constexpr const char* kParamColumns     = "columns";
constexpr const char* kParamTrail       = "trail";
constexpr const char* kParamDensity     = "density";
constexpr const char* kParamMutate      = "mutate";
constexpr const char* kParamFalloff     = "falloff";
constexpr const char* kParamSeed        = "seed";
constexpr const char* kParamSource      = "textSource";
constexpr const char* kParamCharSet     = "characterSet";
constexpr const char* kParamCustomText  = "customText";
constexpr const char* kParamTextFile    = "textFile";
constexpr const char* kParamMirror      = "mirrorGlyphs";
constexpr const char* kParamFontFile    = "fontFile";
constexpr const char* kParamGlyphScale  = "glyphScale";
constexpr const char* kParamTextColour  = "textColour";
constexpr const char* kParamTextOpacity = "textOpacity";
constexpr const char* kParamHeadColour  = "headColour";
constexpr const char* kParamHeadBoost   = "headBoost";
constexpr const char* kParamBackColour  = "backColour";
constexpr const char* kParamBackOpacity = "backOpacity";
constexpr const char* kParamGlow        = "glow";
constexpr const char* kParamMix         = "mix";
constexpr const char* kParamPreset      = "preset";

/// The atlas plus everything derived from it, built once per stream/font
/// change and shared read-only between render threads.
struct StreamData
{
	//What it was built from, so a render can tell whether it is still current.
	int source = -1;
	int charSet = -1;
	std::string customText;
	std::string textFile;
	std::string fontFile;

	std::vector<int> streamSlots; //!< atlas slot per stream position
	downpour::Flow flow = downpour::Flow::Scatter;

	/// The atlas and its mip chain, level 0 first, each level half the size of
	/// the one before. The GPU samples the atlas with mipmapped GL_LINEAR;
	/// minified glyphs alias without the same treatment here.
	std::vector<std::vector<uint8_t>> mips;
	std::vector<int> mipSize;
};

/// One box-filtered mip chain. Power-of-two square input.
void buildMips( StreamData& data, std::vector<uint8_t> base, int size )
{
	data.mips.clear();
	data.mipSize.clear();
	data.mips.push_back( std::move( base ) );
	data.mipSize.push_back( size );

	while( size > 64 )
	{
		const std::vector<uint8_t>& src = data.mips.back();
		const int half                  = size / 2;
		std::vector<uint8_t> next( size_t( half ) * half );
		for( int y = 0; y < half; ++y )
			for( int x = 0; x < half; ++x )
			{
				const int sum = src[ size_t( 2 * y ) * size + 2 * x ]
								+ src[ size_t( 2 * y ) * size + 2 * x + 1 ]
								+ src[ size_t( 2 * y + 1 ) * size + 2 * x ]
								+ src[ size_t( 2 * y + 1 ) * size + 2 * x + 1 ];
				next[ size_t( y ) * half + x ] = uint8_t( ( sum + 2 ) / 4 );
			}
		data.mips.push_back( std::move( next ) );
		data.mipSize.push_back( half );
		size = half;
	}
}

/// Bilinear tap on one mip level, `tx, ty` in level-0 texel coordinates.
float sampleLevel( const StreamData& d, int level, double tx, double ty )
{
	const int size                  = d.mipSize[ size_t( level ) ];
	const std::vector<uint8_t>& img = d.mips[ size_t( level ) ];
	const double scale              = double( size ) / d.mipSize[ 0 ];

	const double fx = tx * scale - 0.5;
	const double fy = ty * scale - 0.5;
	const int x0    = std::clamp( int( std::floor( fx ) ), 0, size - 1 );
	const int y0    = std::clamp( int( std::floor( fy ) ), 0, size - 1 );
	const int x1    = std::min( x0 + 1, size - 1 );
	const int y1    = std::min( y0 + 1, size - 1 );
	const double ax = std::clamp( fx - x0, 0.0, 1.0 );
	const double ay = std::clamp( fy - y0, 0.0, 1.0 );

	const double top    = img[ size_t( y0 ) * size + x0 ] * ( 1.0 - ax ) + img[ size_t( y0 ) * size + x1 ] * ax;
	const double bottom = img[ size_t( y1 ) * size + x0 ] * ( 1.0 - ax ) + img[ size_t( y1 ) * size + x1 ] * ax;
	return float( ( top + ( bottom - top ) * ay ) / 255.0 );
}

/// Everything one render needs, fixed before the threads fan out.
struct RainSetup
{
	downpour::RainState state;
	std::shared_ptr<const StreamData> stream;

	double glyphScale = 1.0;
	bool mirror       = false;

	float textR = 0.62f, textG = 1.0f, textB = 0.68f;
	float textOpacity = 1.0f;
	float headR = 0.9f, headG = 1.0f, headB = 0.95f;
	float headBoost = 1.0f;
	float backR = 0.0f, backG = 0.0f, backB = 0.0f;
	float backOpacity = 1.0f;
	float glow        = 0.0f;
	float mixWithClip = 1.0f;

	bool over = false; //!< the effect variant: rain over the incoming clip
};

/// Ink coverage at `local` (0..1 within the glyph body) for one atlas slot —
/// the CPU mirror of the shader's SampleGlyph, trilinear across the mip chain.
/// `drawnPx` is how many output pixels the glyph body spans, which picks the
/// mip level exactly as the GPU's derivative-driven fetch would.
float sampleGlyph( const StreamData& d, int slot, double localX, double localY, double drawnPx )
{
	using namespace downpour;

	if( localX < 0.0 || localY < 0.0 || localX > 1.0 || localY > 1.0 )
		return 0.0f;

	//This is where the two vertical conventions meet — the grid runs y-down,
	//the atlas y-up — and it is the only place either may change.
	const double uprightX = localX;
	const double uprightY = 1.0 - localY;

	//Half a texel in on every side, so a linear tap at the very edge of the
	//glyph body does not take half its weight from the blank border.
	const double inset = 0.5 / kGlyphPx;
	const double bodyX = inset + ( 1.0 - 2.0 * inset ) * uprightX;
	const double bodyY = inset + ( 1.0 - 2.0 * inset ) * uprightY;

	const int slotX = slot % kAtlasCols;
	const int slotY = slot / kAtlasCols;
	const double tx = slotX * kSlotPx + kBorderPx + bodyX * kGlyphPx;
	const double ty = slotY * kSlotPx + kBorderPx + bodyY * kGlyphPx;

	//Mip level from the minification ratio, clamped into the chain.
	double lod = std::log2( std::max( 1.0, double( kGlyphPx ) / std::max( 1.0, drawnPx ) ) );
	lod        = std::clamp( lod, 0.0, double( d.mips.size() - 1 ) );
	const int level0 = int( lod );
	const int level1 = std::min( level0 + 1, int( d.mips.size() ) - 1 );
	const double mix = lod - level0;

	const float a = sampleLevel( d, level0, tx, ty );
	const float b = sampleLevel( d, level1, tx, ty );
	return float( a + ( b - a ) * mix );
}

class DownpourProcessorBase : public OFX::ImageProcessor
{
public:
	explicit DownpourProcessorBase( OFX::ImageEffect& effect ) :
		OFX::ImageProcessor( effect )
	{
	}

	void setSetup( OFX::Image* src, const RainSetup* v, bool premultipliedValue )
	{
		srcImg        = src;
		setup         = v;
		premultiplied = premultipliedValue;
	}

protected:
	OFX::Image* srcImg     = nullptr; //!< null for the generator
	const RainSetup* setup = nullptr;
	bool premultiplied     = false;
};

template<class PIX, int nComponents, int maxValue>
class DownpourProcessor : public DownpourProcessorBase
{
public:
	explicit DownpourProcessor( OFX::ImageEffect& effect ) :
		DownpourProcessorBase( effect )
	{
	}

	void multiThreadProcessImages( OfxRectI window ) override
	{
		using namespace downpour;

		const OfxRectI bounds = _dstImg->getBounds();
		const int outW        = bounds.x2 - bounds.x1;
		const int outH        = bounds.y2 - bounds.y1;
		const RainSetup& s    = *setup;
		const StreamData& d   = *s.stream;

		const int columns = s.state.columns;
		const int rows    = s.state.rows;

		//Glyph fitting, in pixels: the glyph is square, fitted to the cell's
		//short side, centred on the long one.
		const double cellPxX     = double( outW ) / columns;
		const double cellPxY     = double( outH ) / rows;
		const double shortSide   = std::min( cellPxX, cellPxY );
		const double spanX       = shortSide / cellPxX * std::max( s.glyphScale, 0.001 );
		const double spanY       = shortSide / cellPxY * std::max( s.glyphScale, 0.001 );
		const double drawnPx     = shortSide * std::max( s.glyphScale, 0.001 );

		for( int y = window.y1; y < window.y2; ++y )
		{
			if( _effect.abort() )
				break;

			PIX* dstPix = static_cast<PIX*>( _dstImg->getPixelAddress( window.x1, y ) );

			//uv origin is bottom-left; the grid's is top-left, to match
			//Rain.cpp and the way anyone describes falling rain.
			const double gridY = ( 1.0 - ( y - bounds.y1 + 0.5 ) / outH ) * rows;
			const int cellY    = std::clamp( int( gridY ), 0, rows - 1 );

			for( int x = window.x1; x < window.x2; ++x, dstPix += nComponents )
			{
				const double gridX = ( x - bounds.x1 + 0.5 ) / outW * columns;
				const int cellX    = std::clamp( int( gridX ), 0, columns - 1 );

				const Cell cell = Evaluate( s.state, cellX, cellY );

				float ink = 0.0f;
				if( cell.brightness > 0.0f )
				{
					const double withinX = gridX - cellX;
					const double withinY = gridY - cellY;

					double localX = ( withinX - 0.5 * ( 1.0 - spanX ) ) / spanX;
					const double localY = ( withinY - 0.5 * ( 1.0 - spanY ) ) / spanY;
					if( s.mirror )
						localX = 1.0 - localX;

					const int position = d.streamSlots.empty()
											 ? kBlankSlot
											 : d.streamSlots[ size_t( cell.stream ) % d.streamSlots.size() ];
					ink = sampleGlyph( d, position, localX, localY, drawnPx );
				}

				//--- composite, exactly as the shader --------------------------
				const float litR = s.textR + ( s.headR - s.textR ) * cell.head * s.headBoost;
				const float litG = s.textG + ( s.headG - s.textG ) * cell.head * s.headBoost;
				const float litB = s.textB + ( s.headB - s.textB ) * cell.head * s.headBoost;

				const float body      = cell.brightness + s.glow * cell.brightness * ( 1.0f - cell.brightness );
				const float textAlpha = std::clamp( ink * body * s.textOpacity, 0.0f, 1.0f );

				double r, g, b, a;
				const double backA = s.backOpacity;

				if( !s.over )
				{
					r = litR * textAlpha + s.backR * backA * ( 1.0 - textAlpha );
					g = litG * textAlpha + s.backG * backA * ( 1.0 - textAlpha );
					b = litB * textAlpha + s.backB * backA * ( 1.0 - textAlpha );
					a = textAlpha + backA * ( 1.0 - textAlpha );
				}
				else
				{
					double clip[ 4 ];
					readClip( x, y, clip );

					//The clip sits under our own background, which sits under
					//the rain.
					const double veiledR = s.backR * backA + clip[ 0 ] * ( 1.0 - backA );
					const double veiledG = s.backG * backA + clip[ 1 ] * ( 1.0 - backA );
					const double veiledB = s.backB * backA + clip[ 2 ] * ( 1.0 - backA );
					const double veiledA = backA + clip[ 3 ] * ( 1.0 - backA );

					const double rainedR = litR * textAlpha + veiledR * ( 1.0 - textAlpha );
					const double rainedG = litG * textAlpha + veiledG * ( 1.0 - textAlpha );
					const double rainedB = litB * textAlpha + veiledB * ( 1.0 - textAlpha );
					const double rainedA = textAlpha + veiledA * ( 1.0 - textAlpha );

					r = clip[ 0 ] + ( rainedR - clip[ 0 ] ) * s.mixWithClip;
					g = clip[ 1 ] + ( rainedG - clip[ 1 ] ) * s.mixWithClip;
					b = clip[ 2 ] + ( rainedB - clip[ 2 ] ) * s.mixWithClip;
					a = clip[ 3 ] + ( rainedA - clip[ 3 ] ) * s.mixWithClip;
				}

				r = std::min( r, a );
				g = std::min( g, a );
				b = std::min( b, a );

				if( !premultiplied && nComponents == 4 && a > 0.0 )
				{
					r /= a;
					g /= a;
					b /= a;
				}

				dstPix[ 0 ] = quantise( r );
				dstPix[ 1 ] = quantise( g );
				dstPix[ 2 ] = quantise( b );
				if( nComponents == 4 )
					dstPix[ 3 ] = quantise( a );
			}
		}
	}

private:
	/// The incoming clip's pixel, premultiplied RGBA in 0..1.
	void readClip( int x, int y, double out[ 4 ] ) const
	{
		const PIX* srcPix = srcImg ? static_cast<const PIX*>( srcImg->getPixelAddress( x, y ) ) : nullptr;
		if( !srcPix )
		{
			out[ 0 ] = out[ 1 ] = out[ 2 ] = out[ 3 ] = 0.0;
			return;
		}

		out[ 0 ] = srcPix[ 0 ] / double( maxValue );
		out[ 1 ] = srcPix[ 1 ] / double( maxValue );
		out[ 2 ] = srcPix[ 2 ] / double( maxValue );
		out[ 3 ] = nComponents == 4 ? srcPix[ 3 ] / double( maxValue ) : 1.0;

		if( !premultiplied && nComponents == 4 )
		{
			out[ 0 ] *= out[ 3 ];
			out[ 1 ] *= out[ 3 ];
			out[ 2 ] *= out[ 3 ];
		}
	}

	static PIX quantise( double v )
	{
		if( maxValue == 1 )
			return PIX( v );

		v = std::clamp( v, 0.0, 1.0 );
		return PIX( v * maxValue + 0.5 );
	}
};

class DownpourPlugin : public OFX::ImageEffect
{
public:
	DownpourPlugin( OfxImageEffectHandle handle, bool overVariant ) :
		OFX::ImageEffect( handle ),
		over( overVariant )
	{
		dstClip = fetchClip( kOfxImageEffectOutputClipName );
		if( over )
			srcClip = fetchClip( kOfxImageEffectSimpleSourceClipName );

		speed       = fetchDoubleParam( kParamSpeed );
		direction   = fetchChoiceParam( kParamDirection );
		columns     = fetchDoubleParam( kParamColumns );
		trail       = fetchDoubleParam( kParamTrail );
		density     = fetchDoubleParam( kParamDensity );
		mutate      = fetchDoubleParam( kParamMutate );
		falloff     = fetchDoubleParam( kParamFalloff );
		seed        = fetchDoubleParam( kParamSeed );
		source      = fetchChoiceParam( kParamSource );
		charSet     = fetchChoiceParam( kParamCharSet );
		customText  = fetchStringParam( kParamCustomText );
		textFile    = fetchStringParam( kParamTextFile );
		mirror      = fetchBooleanParam( kParamMirror );
		fontFile    = fetchStringParam( kParamFontFile );
		glyphScale  = fetchDoubleParam( kParamGlyphScale );
		textColour  = fetchRGBParam( kParamTextColour );
		textOpacity = fetchDoubleParam( kParamTextOpacity );
		headColour  = fetchRGBParam( kParamHeadColour );
		headBoost   = fetchDoubleParam( kParamHeadBoost );
		backColour  = fetchRGBParam( kParamBackColour );
		backOpacity = fetchDoubleParam( kParamBackOpacity );
		glow        = fetchDoubleParam( kParamGlow );
		if( over )
			mixParam = fetchDoubleParam( kParamMix );
		preset = fetchChoiceParam( kParamPreset );
	}

	void changedParam( const OFX::InstanceChangedArgs& args, const std::string& paramName ) override
	{
		// The About links open a browser and change nothing about the render.
		if( stoatworks::about::ofx::changedParam( args, paramName ) )
			return;

		using namespace downpour::presets;

		if( paramName == kParamPreset )
		{
			int chosen = 0;
			preset->getValue( chosen );
			if( chosen <= 0 || chosen > kCount || applyingPreset )
				return;

			// The copy IS the preset — same table as the FFGL build, same 0..1
			// space. One edit block so undo takes the whole preset back at once.
			const Preset& p = kPresets[ chosen - 1 ];
			applyingPreset  = true;
			beginEditBlock( "Preset" );
			setDouble( speed, p.v[ kSpeed ] );
			setChoice( direction, p.v[ kDirection ] );
			setDouble( columns, p.v[ kColumns ] );
			setDouble( trail, p.v[ kTrail ] );
			setDouble( density, p.v[ kDensity ] );
			setDouble( mutate, p.v[ kMutate ] );
			setDouble( falloff, p.v[ kFalloff ] );
			setChoice( source, p.v[ kSource ] );
			setChoice( charSet, p.v[ kCharSet ] );
			setBool( mirror, p.v[ kMirror ] );
			setDouble( glyphScale, p.v[ kGlyphScale ] );
			setRGB( textColour, p.v[ kTextR ], p.v[ kTextG ], p.v[ kTextB ] );
			setDouble( textOpacity, p.v[ kTextOpacity ] );
			setRGB( headColour, p.v[ kHeadR ], p.v[ kHeadG ], p.v[ kHeadB ] );
			setDouble( headBoost, p.v[ kHeadBoost ] );
			setRGB( backColour, p.v[ kBackR ], p.v[ kBackG ], p.v[ kBackB ] );
			setDouble( backOpacity, p.v[ kBackOpacity ] );
			setDouble( glow, p.v[ kGlow ] );
			endEditBlock();
			applyingPreset = false;
			return;
		}

		// Editing a covered control while a preset is active hands control back
		// to the sliders. Judged by value, not by the change reason: hosts are
		// not consistent about reasons, but "still equal to the preset" is
		// unambiguous and also absorbs the host echoing our own setValues.
		if( applyingPreset || args.reason == OFX::eChangeTime )
			return;

		int active = 0;
		preset->getValue( active );
		if( active <= 0 || active > kCount )
			return;

		const Preset& p    = kPresets[ active - 1 ];
		const bool covered =
			( paramName == kParamSpeed && doubleDiffers( speed, p.v[ kSpeed ] ) ) ||
			( paramName == kParamDirection && choiceDiffers( direction, p.v[ kDirection ] ) ) ||
			( paramName == kParamColumns && doubleDiffers( columns, p.v[ kColumns ] ) ) ||
			( paramName == kParamTrail && doubleDiffers( trail, p.v[ kTrail ] ) ) ||
			( paramName == kParamDensity && doubleDiffers( density, p.v[ kDensity ] ) ) ||
			( paramName == kParamMutate && doubleDiffers( mutate, p.v[ kMutate ] ) ) ||
			( paramName == kParamFalloff && doubleDiffers( falloff, p.v[ kFalloff ] ) ) ||
			( paramName == kParamSource && choiceDiffers( source, p.v[ kSource ] ) ) ||
			( paramName == kParamCharSet && choiceDiffers( charSet, p.v[ kCharSet ] ) ) ||
			( paramName == kParamMirror && boolDiffers( mirror, p.v[ kMirror ] ) ) ||
			( paramName == kParamGlyphScale && doubleDiffers( glyphScale, p.v[ kGlyphScale ] ) ) ||
			( paramName == kParamTextColour && rgbDiffers( textColour, p.v[ kTextR ], p.v[ kTextG ], p.v[ kTextB ] ) ) ||
			( paramName == kParamTextOpacity && doubleDiffers( textOpacity, p.v[ kTextOpacity ] ) ) ||
			( paramName == kParamHeadColour && rgbDiffers( headColour, p.v[ kHeadR ], p.v[ kHeadG ], p.v[ kHeadB ] ) ) ||
			( paramName == kParamHeadBoost && doubleDiffers( headBoost, p.v[ kHeadBoost ] ) ) ||
			( paramName == kParamBackColour && rgbDiffers( backColour, p.v[ kBackR ], p.v[ kBackG ], p.v[ kBackB ] ) ) ||
			( paramName == kParamBackOpacity && doubleDiffers( backOpacity, p.v[ kBackOpacity ] ) ) ||
			( paramName == kParamGlow && doubleDiffers( glow, p.v[ kGlow ] ) );

		if( covered )
		{
			applyingPreset = true;
			preset->setValue( 0 );
			applyingPreset = false;
		}
	}

	void render( const OFX::RenderArguments& args ) override
	{
		std::unique_ptr<OFX::Image> dst( dstClip->fetchImage( args.time ) );
		std::unique_ptr<OFX::Image> src;
		if( over && srcClip && srcClip->isConnected() )
			src.reset( srcClip->fetchImage( args.time ) );

		const bool premultiplied =
			over && srcClip ? srcClip->getPreMultiplication() == OFX::eImagePreMultiplied
							: dstClip->getPreMultiplication() == OFX::eImagePreMultiplied;

		const OFX::BitDepthEnum depth       = dst->getPixelDepth();
		const OFX::PixelComponentEnum comps = dst->getPixelComponents();

		if( comps != OFX::ePixelComponentRGBA && comps != OFX::ePixelComponentRGB )
			OFX::throwSuiteStatusException( kOfxStatErrUnsupported );

		RainSetup setup;
		buildSetup( args, *dst, setup );

		switch( depth )
		{
		case OFX::eBitDepthUByte:
			comps == OFX::ePixelComponentRGBA
				? run<DownpourProcessor<unsigned char, 4, 255>>( args, dst.get(), src.get(), setup, premultiplied )
				: run<DownpourProcessor<unsigned char, 3, 255>>( args, dst.get(), src.get(), setup, premultiplied );
			break;
		case OFX::eBitDepthUShort:
			comps == OFX::ePixelComponentRGBA
				? run<DownpourProcessor<unsigned short, 4, 65535>>( args, dst.get(), src.get(), setup, premultiplied )
				: run<DownpourProcessor<unsigned short, 3, 65535>>( args, dst.get(), src.get(), setup, premultiplied );
			break;
		case OFX::eBitDepthFloat:
			comps == OFX::ePixelComponentRGBA
				? run<DownpourProcessor<float, 4, 1>>( args, dst.get(), src.get(), setup, premultiplied )
				: run<DownpourProcessor<float, 3, 1>>( args, dst.get(), src.get(), setup, premultiplied );
			break;
		default:
			OFX::throwSuiteStatusException( kOfxStatErrUnsupported );
		}
	}

private:
	void buildSetup( const OFX::RenderArguments& args, OFX::Image& dst, RainSetup& setup )
	{
		using namespace downpour;

		const double t = args.time;

		const OfxRectI b = dst.getBounds();
		const int outW   = b.x2 - b.x1;
		const int outH   = b.y2 - b.y1;

		//OFX time is in frames; the rain wants seconds. The clip's frame rate
		//makes the conversion, and a fallback of 24 keeps a degenerate host
		//animating rather than frozen.
		double fps = dstClip->getFrameRate();
		if( !( fps > 0.0 ) )
			fps = 24.0;

		setup.over = over;

		const float seconds = float( t / fps );
		setup.state.columns = ColumnsFromParam( float( columns->getValueAtTime( t ) ) );
		setup.state.rows    = RowsForAspect( setup.state.columns, outW, outH );
		setup.state.speed   = SpeedFromParam( float( speed->getValueAtTime( t ) ) );
		setup.state.trail   = TrailFromParam( float( trail->getValueAtTime( t ) ) );
		setup.state.density = float( density->getValueAtTime( t ) );
		// Scaled by Speed so churn calms with the rain — see kMutateReferenceSpeed.
		setup.state.mutate  = MutateFromParam( float( mutate->getValueAtTime( t ) ) ) * setup.state.speed / kMutateReferenceSpeed;

		// The plain products, deliberately. The FFGL build anchors these so that
		// nudging Speed or Mutate live does not teleport the rain, and that anchor
		// is a running carry which needs frames to arrive in order. This host
		// renders arbitrary times in arbitrary order and can keyframe both
		// controls, so a carry here would make a frame depend on which frames
		// happened to be rendered before it. A pure function of time is the right
		// answer for a timeline; see Downpour.h.
		setup.state.travel      = seconds * setup.state.speed;
		setup.state.mutateTicks = seconds * setup.state.mutate;
		setup.state.falloff = FalloffFromParam( float( falloff->getValueAtTime( t ) ) );
		setup.state.seed    = SeedFromParam( float( seed->getValueAtTime( t ) ) );

		int directionValue = 0;
		direction->getValueAtTime( t, directionValue );
		setup.state.direction = Direction( directionValue );

		int sourceValue = 0, charSetValue = 0;
		source->getValueAtTime( t, sourceValue );
		charSet->getValueAtTime( t, charSetValue );
		std::string customValue, fileValue, fontValue;
		customText->getValueAtTime( t, customValue );
		textFile->getValueAtTime( t, fileValue );
		fontFile->getValueAtTime( t, fontValue );

		setup.stream = streamFor( sourceValue, charSetValue, customValue, fileValue, fontValue );
		setup.state.flow         = setup.stream->flow;
		setup.state.streamLength = int( setup.stream->streamSlots.size() );

		setup.glyphScale = GlyphScaleFromParam( float( glyphScale->getValueAtTime( t ) ) );
		setup.mirror     = mirror->getValueAtTime( t );

		double r, g, bb;
		textColour->getValueAtTime( t, r, g, bb );
		setup.textR = float( r );
		setup.textG = float( g );
		setup.textB = float( bb );
		headColour->getValueAtTime( t, r, g, bb );
		setup.headR = float( r );
		setup.headG = float( g );
		setup.headB = float( bb );
		backColour->getValueAtTime( t, r, g, bb );
		setup.backR = float( r );
		setup.backG = float( g );
		setup.backB = float( bb );

		setup.textOpacity = float( textOpacity->getValueAtTime( t ) );
		setup.headBoost   = HeadBoostFromParam( float( headBoost->getValueAtTime( t ) ) );
		setup.backOpacity = float( backOpacity->getValueAtTime( t ) );
		setup.glow        = GlowFromParam( float( glow->getValueAtTime( t ) ) );
		setup.mixWithClip = over && mixParam ? float( mixParam->getValueAtTime( t ) ) : 1.0f;
	}

	/// The stream and atlas for these settings, rebuilt only when they change.
	/// Guarded: fully-thread-safe rendering means two renders of one instance
	/// can arrive together.
	std::shared_ptr<const StreamData> streamFor( int sourceValue, int charSetValue,
												 const std::string& customValue,
												 const std::string& fileValue,
												 const std::string& fontValue )
	{
		using namespace downpour;

		std::lock_guard<std::mutex> hold( cacheMutex );

		if( cache && cache->source == sourceValue && cache->charSet == charSetValue
			&& cache->customText == customValue && cache->textFile == fileValue
			&& cache->fontFile == fontValue )
			return cache;

		auto built     = std::make_shared<StreamData>();
		built->source  = sourceValue;
		built->charSet = charSetValue;
		built->customText = customValue;
		built->textFile   = fileValue;
		built->fontFile   = fontValue;

		const Stream stream = BuildStream( Source( sourceValue ), CharSet( charSetValue ),
										   customValue, fileValue );
		built->flow = stream.flow;

		Typeface face;
		if( !fontValue.empty() )
			face.Load( fontValue );

		std::vector<uint8_t> image;
		std::unordered_map<uint32_t, int> slots;
		int fromFont = 0, fromBuiltin = 0, missing = 0;
		face.BuildAtlas( stream.codepoints, image, slots, fromFont, fromBuiltin, missing );

		built->streamSlots.reserve( stream.codepoints.size() );
		for( uint32_t codepoint : stream.codepoints )
		{
			auto found = slots.find( codepoint );
			built->streamSlots.push_back( found != slots.end() ? found->second : kBlankSlot );
		}
		if( built->streamSlots.empty() )
			built->streamSlots.push_back( kBlankSlot );

		buildMips( *built, std::move( image ), kAtlasPx );

		cache = std::move( built );
		return cache;
	}

	template<class Processor>
	void run( const OFX::RenderArguments& args, OFX::Image* dst, OFX::Image* src,
			  const RainSetup& setup, bool premultiplied )
	{
		Processor processor( *this );
		processor.setDstImg( dst );
		processor.setSetup( src, &setup, premultiplied );
		processor.setRenderWindow( args.renderWindow );
		processor.process();
	}

	const bool over;
	OFX::Clip* dstClip             = nullptr;
	OFX::Clip* srcClip             = nullptr;
	OFX::DoubleParam* speed        = nullptr;
	OFX::ChoiceParam* direction    = nullptr;
	OFX::DoubleParam* columns      = nullptr;
	OFX::DoubleParam* trail        = nullptr;
	OFX::DoubleParam* density      = nullptr;
	OFX::DoubleParam* mutate       = nullptr;
	OFX::DoubleParam* falloff      = nullptr;
	OFX::DoubleParam* seed         = nullptr;
	OFX::ChoiceParam* source       = nullptr;
	OFX::ChoiceParam* charSet      = nullptr;
	OFX::StringParam* customText   = nullptr;
	OFX::StringParam* textFile     = nullptr;
	OFX::BooleanParam* mirror      = nullptr;
	OFX::StringParam* fontFile     = nullptr;
	OFX::DoubleParam* glyphScale   = nullptr;
	OFX::RGBParam* textColour      = nullptr;
	OFX::DoubleParam* textOpacity  = nullptr;
	OFX::RGBParam* headColour      = nullptr;
	OFX::DoubleParam* headBoost    = nullptr;
	OFX::RGBParam* backColour      = nullptr;
	OFX::DoubleParam* backOpacity  = nullptr;
	OFX::DoubleParam* glow         = nullptr;
	OFX::DoubleParam* mixParam     = nullptr;
	OFX::ChoiceParam* preset       = nullptr;

	// The preset table is plain floats; these give each param type its
	// reading of one. Option values are element indices, booleans are 0/1.
	static bool doubleDiffers( OFX::DoubleParam* p, float v )
	{
		double current = 0.0;
		p->getValue( current );
		return std::fabs( current - double( v ) ) > 1e-4;
	}
	static bool boolDiffers( OFX::BooleanParam* p, float v )
	{
		bool current = false;
		p->getValue( current );
		return current != ( v > 0.5f );
	}
	static bool choiceDiffers( OFX::ChoiceParam* p, float v )
	{
		int current = 0;
		p->getValue( current );
		return current != int( std::lround( v ) );
	}
	static bool rgbDiffers( OFX::RGBParam* p, float r, float g, float b )
	{
		double cr = 0.0, cg = 0.0, cb = 0.0;
		p->getValue( cr, cg, cb );
		return std::fabs( cr - double( r ) ) > 1e-4 || std::fabs( cg - double( g ) ) > 1e-4
			   || std::fabs( cb - double( b ) ) > 1e-4;
	}
	static void setDouble( OFX::DoubleParam* p, float v )
	{
		if( doubleDiffers( p, v ) )
			p->setValue( double( v ) );
	}
	static void setBool( OFX::BooleanParam* p, float v )
	{
		if( boolDiffers( p, v ) )
			p->setValue( v > 0.5f );
	}
	static void setChoice( OFX::ChoiceParam* p, float v )
	{
		if( choiceDiffers( p, v ) )
			p->setValue( int( std::lround( v ) ) );
	}
	static void setRGB( OFX::RGBParam* p, float r, float g, float b )
	{
		if( rgbDiffers( p, r, g, b ) )
			p->setValue( double( r ), double( g ), double( b ) );
	}

	/// True while our own setValues are in flight, so the resulting
	/// changedParam callbacks are not mistaken for the operator editing.
	bool applyingPreset = false;

	std::mutex cacheMutex;
	std::shared_ptr<const StreamData> cache;
};

OFX::DoubleParamDescriptor* defineSlider( OFX::ImageEffectDescriptor& desc, OFX::PageParamDescriptor* page,
										  const char* name, const char* label, const char* hint, double def )
{
	OFX::DoubleParamDescriptor* p = desc.defineDoubleParam( name );
	p->setLabels( label, label, label );
	p->setHint( hint );
	p->setRange( 0.0, 1.0 );
	p->setDisplayRange( 0.0, 1.0 );
	p->setDefault( def );
	page->addChild( *p );
	return p;
}

OFX::RGBParamDescriptor* defineColour( OFX::ImageEffectDescriptor& desc, OFX::PageParamDescriptor* page,
									   const char* name, const char* label, const char* hint,
									   double r, double g, double b )
{
	OFX::RGBParamDescriptor* p = desc.defineRGBParam( name );
	p->setLabels( label, label, label );
	p->setHint( hint );
	p->setDefault( r, g, b );
	page->addChild( *p );
	return p;
}

void describeCommon( OFX::ImageEffectDescriptor& desc, const char* name )
{
	desc.setLabels( name, name, name );
	desc.setPluginGrouping( kPluginGrouping );
	desc.setPluginDescription( kPluginDescription );

	desc.addSupportedBitDepth( OFX::eBitDepthUByte );
	desc.addSupportedBitDepth( OFX::eBitDepthUShort );
	desc.addSupportedBitDepth( OFX::eBitDepthFloat );

	// A cell is a pure function of (column, row, time): frames render in any
	// order, alone, and concurrently. Cells read pixels across the whole
	// frame's grid, so no tiles.
	desc.setSupportsTiles( false );
	desc.setTemporalClipAccess( false );
	desc.setRenderThreadSafety( OFX::eRenderFullySafe );
	desc.setSupportsMultiResolution( true );
}

void describeParams( OFX::ImageEffectDescriptor& desc, bool over )
{
	using namespace downpour;

	OFX::PageParamDescriptor* page = desc.definePageParam( "Controls" );

	// Factory presets, from the same table the FFGL build reads (Presets.h).
	// Custom is not a preset: it means the sliders are the truth.
	OFX::ChoiceParamDescriptor* presetParam = desc.defineChoiceParam( kParamPreset );
	presetParam->setLabels( "Preset", "Preset", "Preset" );
	presetParam->setHint( "Named rains. Picking one sets the covered controls; editing any "
	                      "of them afterwards falls back to Custom." );
	presetParam->appendOption( "Custom" );
	for( int i = 0; i < downpour::presets::kCount; ++i )
		presetParam->appendOption( downpour::presets::kPresets[ i ].name );
	presetParam->setDefault( 0 );
	presetParam->setIsPersistant( true );
	presetParam->setEvaluateOnChange( false );//the copied values re-render; the label itself does not
	presetParam->setAnimates( false );
	page->addChild( *presetParam );

	OFX::GroupParamDescriptor* rain = desc.defineGroupParam( "Rain" );
	rain->setLabels( "Rain", "Rain", "Rain" );

	defineSlider( desc, page, kParamSpeed, "Speed", "Rows per second, 0.5 to 45, exponential.", 0.5 )
		->setParent( *rain );

	OFX::ChoiceParamDescriptor* directionParam = desc.defineChoiceParam( kParamDirection );
	directionParam->setLabels( "Direction", "Direction", "Direction" );
	directionParam->setHint( "Which way the rain runs." );
	directionParam->appendOption( "Down" );
	directionParam->appendOption( "Up" );
	directionParam->appendOption( "Right" );
	directionParam->appendOption( "Left" );
	directionParam->setDefault( 0 );
	directionParam->setParent( *rain );
	page->addChild( *directionParam );

	defineSlider( desc, page, kParamColumns, "Columns", "Columns across the frame, 8 to 240, exponential.", 0.5 )
		->setParent( *rain );
	defineSlider( desc, page, kParamTrail, "Trail", "Trail length as a fraction of the run.", 0.5 )
		->setParent( *rain );
	defineSlider( desc, page, kParamDensity, "Density", "Fraction of columns carrying a drop at all.", 0.85 )
		->setParent( *rain );
	defineSlider( desc, page, kParamMutate, "Mutate", "Glyph changes per second; 0 is a still character.", 0.5 )
		->setParent( *rain );
	defineSlider( desc, page, kParamFalloff, "Falloff", "Brightness exponent along the trail; 0.5 is 1.", 0.5 )
		->setParent( *rain );
	defineSlider( desc, page, kParamSeed, "Seed", "A different rain, 1 to 9999.", 0.0 )
		->setParent( *rain );

	OFX::GroupParamDescriptor* text = desc.defineGroupParam( "Text" );
	text->setLabels( "Text", "Text", "Text" );

	OFX::ChoiceParamDescriptor* sourceParam = desc.defineChoiceParam( kParamSource );
	sourceParam->setLabels( "Source", "Source", "Source" );
	sourceParam->setHint( "What to draw: random characters, generated code, or a document that plays out." );
	for( int i = 0; i < int( Source::Count ); ++i )
		sourceParam->appendOption( SourceName( Source( i ) ) );
	sourceParam->setDefault( 0 );
	sourceParam->setParent( *text );
	page->addChild( *sourceParam );

	OFX::ChoiceParamDescriptor* charSetParam = desc.defineChoiceParam( kParamCharSet );
	charSetParam->setLabels( "Character Set", "Character Set", "Character Set" );
	charSetParam->setHint( "The alphabet Junk draws from." );
	for( int i = 0; i < int( CharSet::Count ); ++i )
		charSetParam->appendOption( CharSetName( CharSet( i ) ) );
	charSetParam->setDefault( 0 );
	charSetParam->setParent( *text );
	page->addChild( *charSetParam );

	OFX::StringParamDescriptor* customParam = desc.defineStringParam( kParamCustomText );
	customParam->setLabels( "Custom Text", "Custom Text", "Custom Text" );
	customParam->setHint( "The text for the Custom Text source." );
	customParam->setDefault( "" );
	customParam->setParent( *text );
	page->addChild( *customParam );

	OFX::StringParamDescriptor* fileParam = desc.defineStringParam( kParamTextFile );
	fileParam->setLabels( "Text File", "Text File", "Text File" );
	fileParam->setHint( "A .txt for the Text File source. A missing file falls back to generated code." );
	fileParam->setStringType( OFX::eStringTypeFilePath );
	fileParam->setDefault( "" );
	fileParam->setParent( *text );
	page->addChild( *fileParam );

	OFX::BooleanParamDescriptor* mirrorParam = desc.defineBooleanParam( kParamMirror );
	mirrorParam->setLabels( "Mirror Glyphs", "Mirror Glyphs", "Mirror Glyphs" );
	mirrorParam->setHint( "The film's glyphs are mirrored." );
	mirrorParam->setDefault( false );
	mirrorParam->setParent( *text );
	page->addChild( *mirrorParam );

	OFX::GroupParamDescriptor* font = desc.defineGroupParam( "Font" );
	font->setLabels( "Font", "Font", "Font" );

	OFX::StringParamDescriptor* fontParam = desc.defineStringParam( kParamFontFile );
	fontParam->setLabels( "Font File", "Font File", "Font File" );
	fontParam->setHint( "A .ttf/.otf/.ttc to draw with. Empty, or any character it cannot draw, "
						"uses the built-in bitmap face." );
	fontParam->setStringType( OFX::eStringTypeFilePath );
	fontParam->setDefault( "" );
	fontParam->setParent( *font );
	page->addChild( *fontParam );

	defineSlider( desc, page, kParamGlyphScale, "Glyph Scale", "Glyph size within its cell; 0.75 fills it.", 0.75 )
		->setParent( *font );

	OFX::GroupParamDescriptor* colour = desc.defineGroupParam( "Colour" );
	colour->setLabels( "Colour", "Colour", "Colour" );

	defineColour( desc, page, kParamTextColour, "Text", "The trail's colour.", 0.62, 1.0, 0.68 )
		->setParent( *colour );
	defineSlider( desc, page, kParamTextOpacity, "Text Opacity", "", 1.0 )->setParent( *colour );
	defineColour( desc, page, kParamHeadColour, "Head", "The leading character's colour.", 0.9, 1.0, 0.95 )
		->setParent( *colour );
	defineSlider( desc, page, kParamHeadBoost, "Head Boost", "How hard the head colour is pushed.", 1.0 )
		->setParent( *colour );
	defineColour( desc, page, kParamBackColour, "Background", "Behind the rain.", 0.0, 0.0, 0.0 )
		->setParent( *colour );
	defineSlider( desc, page, kParamBackOpacity, "Background Opacity",
				  over ? "How much the background veils the footage." : "0 rains on transparency.", over ? 0.0 : 1.0 )
		->setParent( *colour );
	defineSlider( desc, page, kParamGlow, "Glow", "A lift on the trail's own brightness, 0 to 2.", 0.0 )
		->setParent( *colour );

	if( over )
	{
		OFX::GroupParamDescriptor* output = desc.defineGroupParam( "Output" );
		output->setLabels( "Output", "Output", "Output" );
		defineSlider( desc, page, kParamMix, "Mix", "Dry/wet with the untouched clip.", 1.0 )
			->setParent( *output );
	}

	// The Stoatworks About block: a read-only credit line and one push button
	// per link, in a group that starts folded. Last, so it sits under the
	// effect's own controls.
	stoatworks::about::ofx::describe( desc, page );
}

} // namespace

//---------------------------------------------------------------------------
// "Downpour": the generator.
//---------------------------------------------------------------------------
mDeclarePluginFactory( DownpourSourceFactory, {}, {} );

void DownpourSourceFactory::describe( OFX::ImageEffectDescriptor& desc )
{
	describeCommon( desc, "Downpour" );
	desc.addSupportedContext( OFX::eContextGenerator );
	desc.addSupportedContext( OFX::eContextGeneral );
}

void DownpourSourceFactory::describeInContext( OFX::ImageEffectDescriptor& desc, OFX::ContextEnum )
{
	OFX::ClipDescriptor* dstClip = desc.defineClip( kOfxImageEffectOutputClipName );
	dstClip->addSupportedComponent( OFX::ePixelComponentRGBA );
	dstClip->addSupportedComponent( OFX::ePixelComponentRGB );
	dstClip->setSupportsTiles( false );

	describeParams( desc, false );
}

OFX::ImageEffect* DownpourSourceFactory::createInstance( OfxImageEffectHandle handle, OFX::ContextEnum )
{
	return new DownpourPlugin( handle, false );
}

//---------------------------------------------------------------------------
// "Downpour Over": the effect.
//---------------------------------------------------------------------------
mDeclarePluginFactory( DownpourOverFactory, {}, {} );

void DownpourOverFactory::describe( OFX::ImageEffectDescriptor& desc )
{
	describeCommon( desc, "Downpour Over" );
	desc.addSupportedContext( OFX::eContextFilter );
	desc.addSupportedContext( OFX::eContextGeneral );
}

void DownpourOverFactory::describeInContext( OFX::ImageEffectDescriptor& desc, OFX::ContextEnum )
{
	OFX::ClipDescriptor* srcClip = desc.defineClip( kOfxImageEffectSimpleSourceClipName );
	srcClip->addSupportedComponent( OFX::ePixelComponentRGBA );
	srcClip->addSupportedComponent( OFX::ePixelComponentRGB );
	srcClip->setSupportsTiles( false );

	OFX::ClipDescriptor* dstClip = desc.defineClip( kOfxImageEffectOutputClipName );
	dstClip->addSupportedComponent( OFX::ePixelComponentRGBA );
	dstClip->addSupportedComponent( OFX::ePixelComponentRGB );
	dstClip->setSupportsTiles( false );

	describeParams( desc, true );
}

OFX::ImageEffect* DownpourOverFactory::createInstance( OfxImageEffectHandle handle, OFX::ContextEnum )
{
	return new DownpourPlugin( handle, true );
}

void OFX::Plugin::getPluginIDs( OFX::PluginFactoryArray& ids )
{
	// Deliberately leaked: a by-value static would register an exit-time
	// destructor inside this module, and a host that dlclose()s the bundle
	// before process exit then jumps through a dangling pointer.
	static DownpourSourceFactory* sourceFactory =
		new DownpourSourceFactory( kSourceIdentifier, PLUGIN_VERSION_MAJOR, PLUGIN_VERSION_MINOR );
	static DownpourOverFactory* overFactory =
		new DownpourOverFactory( kOverIdentifier, PLUGIN_VERSION_MAJOR, PLUGIN_VERSION_MINOR );
	ids.push_back( sourceFactory );
	ids.push_back( overFactory );
}
