/**
 * Downpour — browser demo.
 *
 * `kVertexShader` and `kRainShader` below are copied unedited from
 * `source/Shaders.cpp`, including the `#ifdef DOWNPOUR_OVER_INPUT` branch — the
 * two bundles really are one shader compiled twice, and the picker in the
 * transport bar compiles it both ways here for the same reason.
 *
 * `Controls.cpp` (the 0..1 conversions), `Glyphs.cpp` (the atlas), `Rain.cpp`'s
 * salts and `TextSource.cpp` (the character sets and the code generator) are all
 * ported below. The drawn face and the four public domain passages are extracted
 * into `data.js` by `demo/tools/extract_data.py` rather than retyped.
 *
 * The one idea, before any of it: **a cell is a closed-form function of
 * (column, row, host time)**. No simulation state, no feedback buffer. So it
 * cannot drift with the frame rate, it is resolution independent for free, and
 * any frame renders on its own — which is why Step on this page is exact and
 * not an approximation.
 */

import { mountDemo } from './vendor/demo.js';
import { Program, bindTexture } from './vendor/gl.js';
import { FACE, BITMAP_SIZE, WORKS } from './data.js';

//===========================================================================
// Ports of source/Glyphs.h
//===========================================================================

/// Everything — bitmap face and TrueType alike — lands at this size in the
/// atlas, so the shader has one code path.
const GLYPH_PX = 64;
/// One texel of blank on every side. This is what stops a GL_LINEAR fetch at a
/// glyph edge picking up its neighbour.
const BORDER_PX = 1;
const SLOT_PX = GLYPH_PX + 2 * BORDER_PX; // 66
const ATLAS_PX = 2048;
const ATLAS_COLS = Math.floor(ATLAS_PX / SLOT_PX); // 31
const ATLAS_ROWS = Math.floor(ATLAS_PX / SLOT_PX); // 31
const ATLAS_SLOTS = ATLAS_COLS * ATLAS_ROWS;

/// Slot 0 is always blank. Not tidiness: an unknown codepoint, an inactive cell
/// and the gap between two drops all resolve here, which is what lets the shader
/// draw "nothing" without a branch.
const BLANK_SLOT = 0;

const STREAM_WIDTH = 1024;
const MAX_DOCUMENT = 200000;

/// First declaration wins. A codepoint drawn twice is a mistake in
/// GlyphData.cpp rather than a choice.
const BUILTIN_SLOT = new Map();
FACE.forEach(([cp], i) => {
  if (!BUILTIN_SLOT.has(cp)) BUILTIN_SLOT.set(cp, i);
});

function slotOrigin(slot) {
  const col = slot % ATLAS_COLS;
  const row = Math.floor(slot / ATLAS_COLS);
  return [col * SLOT_PX + BORDER_PX, row * SLOT_PX + BORDER_PX];
}

/**
 * The atlas, single channel, 255 for ink.
 *
 * Integer scale, nearest neighbour, centred in the slot. 64 / 12 = 5, so the
 * glyph occupies 60 of the 64 texels with two spare either side; keeping the
 * factor whole is what stops the strokes of a bitmap face coming out uneven
 * widths, which is the thing the eye actually notices.
 */
function buildBuiltinAtlas() {
  const image = new Uint8Array(ATLAS_PX * ATLAS_PX);
  const scale = Math.floor(GLYPH_PX / BITMAP_SIZE);
  const drawn = scale * BITMAP_SIZE;
  const inset = Math.floor((GLYPH_PX - drawn) / 2);

  FACE.forEach(([, ...rows], slot) => {
    if (slot >= ATLAS_SLOTS) return;
    const [originX, originY] = slotOrigin(slot);

    for (let row = 0; row < BITMAP_SIZE; row += 1) {
      for (let col = 0; col < BITMAP_SIZE; col += 1) {
        if (!((rows[row] >> col) & 1)) continue;
        for (let dy = 0; dy < scale; dy += 1) {
          const y = originY + inset + row * scale + dy;
          const line = y * ATLAS_PX;
          for (let dx = 0; dx < scale; dx += 1) {
            image[line + originX + inset + col * scale + dx] = 255;
          }
        }
      }
    }
  });

  return image;
}

//===========================================================================
// Ports of source/Controls.cpp
//===========================================================================

const saturate = (v) => (v < 0 ? 0 : v > 1 ? 1 : v);
/// Geometric: equal slider travel gives equal *ratio*, which is how the eye
/// reads a density or a rate.
const exponential = (v, low, high) => low * (high / low) ** saturate(v);
const linear = (v, low, high) => low + (high - low) * saturate(v);

const columnsFromParam = (v) => Math.round(exponential(v, 8, 240));

function rowsForAspect(columns, width, height) {
  if (columns <= 0 || width <= 0 || height <= 0) return 1;
  // Square cells: a cell's width is 1/columns of the frame, so the number of
  // rows that keeps it square is however many of that width fit down the height.
  const cellWidth = width / columns;
  return Math.max(1, Math.round(height / cellWidth));
}

const speedFromParam = (v) => exponential(v, 0.5, 45);
const trailFromParam = (v) => linear(v, 0.04, 1);

function mutateFromParam(v) {
  // Below a twentieth of the travel the answer is exactly zero. Without the
  // dead zone the bottom of an exponential range is 0.1 changes a second, which
  // looks like no mutation but is not — and shows up as a glyph flicking over
  // once every ten seconds, which reads as a bug.
  const t = saturate(v);
  if (t < 0.05) return 0;
  return exponential((t - 0.05) / 0.95, 0.5, 30);
}

/// Centred on 1: dragging away from the middle in either direction is a visible
/// change of the same size, which a linear 0.4..4 would not be.
const falloffFromParam = (v) => exponential(v, 0.4, 4);

/// Quantised on purpose. The seed picks a rain, not a position in a continuum,
/// and a slider that changes the answer on every pixel of travel cannot be set
/// to the same thing twice.
const seedFromParam = (v) => (1 + Math.round(saturate(v) * 9998)) >>> 0;

function glyphScaleFromParam(v) {
  const t = saturate(v);
  // Two straight segments meeting at 1.0 at three quarters of the travel.
  if (t <= 0.75) return linear(t / 0.75, 0.25, 1);
  return linear((t - 0.75) / 0.25, 1, 1.6);
}

const headBoostFromParam = saturate;
const glowFromParam = (v) => saturate(v) * 2;

//===========================================================================
// Ports of source/Rain.cpp — the hash, for the code generator
//===========================================================================

function hash(x) {
  x = x >>> 0;
  x ^= x >>> 16;
  x = Math.imul(x, 0x7feb352d) >>> 0;
  x ^= x >>> 15;
  x = Math.imul(x, 0x846ca68b) >>> 0;
  x ^= x >>> 16;
  return x >>> 0;
}

//===========================================================================
// Ports of source/TextSource.cpp
//===========================================================================

const SOURCE_NAMES = [
  'Junk', 'Code', 'Alice in Wonderland', 'Plato - The Cave',
  'Hamlet III.i', 'Descartes - The Cogito', 'Custom Text',
];

const CHARSET_NAMES = ['Katakana', 'Katakana + Digits', 'Digits', 'Hex', 'Binary', 'ASCII', 'Latin'];

const range = (first, last) => {
  const out = [];
  for (let c = first; c <= last; c += 1) out.push(c);
  return out;
};

/// Half-width katakana, U+FF66..U+FF9D, minus the punctuation at the bottom of
/// the block. These are the characters the built-in face draws.
function katakana() {
  return [0xff66, ...range(0xff71, 0xff9d), 0xff70];
}

function alphabet(set) {
  switch (set) {
    case 0: return katakana();
    case 1: return [...katakana(), ...range(0x30, 0x39)];
    case 2: return range(0x30, 0x39);
    case 3: return [...range(0x30, 0x39), ...range(0x41, 0x46)];
    case 4: return range(0x30, 0x31);
    // 33 and not 32: the space is left out of a *scattered* set on purpose. A
    // scattered space is a hole in the middle of a trail, and enough of them
    // make the rain look like it is failing to draw rather than like it is
    // sparse. Density is the control for sparseness.
    case 5: return range(33, 126);
    case 6: return [...range(0x41, 0x5a), ...range(0x61, 0x7a)];
    default: return katakana();
  }
}

/**
 * The code generator.
 *
 * What makes text read as *code* at rain speed is not its grammar — nobody
 * parses a falling column — it is its character mix. Brackets, semicolons,
 * dots, arrows, underscores, hex literals and camelCase are what the eye
 * recognises, and prose has almost none of them.
 */
function generateCode(seed, approximateLength) {
  const TYPES = ['int', 'void', 'float', 'uint32_t', 'bool', 'auto', 'char', 'size_t', 'double', 'static'];
  const NAMES = ['buffer', 'node', 'packet', 'frame', 'state', 'index', 'handle', 'stream', 'cursor',
    'matrix', 'vector', 'chunk', 'socket', 'thread', 'kernel', 'device', 'region', 'offset',
    'header', 'payload', 'channel', 'segment'];
  const VERBS = ['decode', 'resolve', 'flush', 'commit', 'acquire', 'map', 'encode', 'advance',
    'release', 'reset', 'probe', 'align', 'compute', 'dispatch', 'collapse', 'unwind'];
  const OPS = ['==', '!=', '<=', '>=', '&&', '||', '<<', '>>', '+=', '->'];
  const KEYS = ['if', 'while', 'for', 'switch', 'return', 'else', 'break', 'continue', 'case', 'goto'];

  // The rain's own hash, so the generator is reproducible on any machine.
  let state = seed === 0 ? 1 : seed;
  const next = (modulus) => {
    state = hash(state);
    return modulus > 0 ? state % modulus : 0;
  };

  const hex = (v, width) => v.toString(16).toUpperCase().padStart(width, '0');

  let out = '';
  while (out.length < approximateLength) {
    switch (next(6)) {
      case 0:
        out += `${TYPES[next(TYPES.length)]} ${NAMES[next(NAMES.length)]}_${next(64)} = 0x${hex(next(65536), 4)}; `;
        break;
      case 1:
        out += `if (${NAMES[next(NAMES.length)]} ${OPS[next(OPS.length)]} ${next(512)}) { ${VERBS[next(VERBS.length)]}(${NAMES[next(NAMES.length)]}); } `;
        break;
      case 2:
        out += `${NAMES[next(NAMES.length)]}->${NAMES[next(NAMES.length)]}[${next(256)}] = ${VERBS[next(VERBS.length)]}(${NAMES[next(NAMES.length)]}, ${next(1024)}); `;
        break;
      case 3:
        out += `for (int i = 0; i < ${next(4096)}; ++i) { `;
        break;
      case 4:
        out += `${TYPES[next(TYPES.length)]} ${VERBS[next(VERBS.length)]}(${TYPES[next(TYPES.length)]}* ${NAMES[next(NAMES.length)]}); `;
        break;
      default:
        out += `${KEYS[next(KEYS.length)]} ${NAMES[next(NAMES.length)]}_${next(32)} ${OPS[next(OPS.length)]} 0x${hex(next(256), 2)}; } `;
        break;
    }
  }
  return out;
}

const FLOW_SCATTER = 0;
const FLOW_SEQUENCE = 1;

/**
 * @returns {{codepoints: number[], flow: number}}
 */
function buildStream(source, set, customText, seed) {
  const fromText = (text, flow) => ({
    codepoints: [...text].map((ch) => ch.codePointAt(0)).slice(0, MAX_DOCUMENT),
    flow,
  });

  switch (source) {
    case 0: return { codepoints: alphabet(set), flow: FLOW_SCATTER };
    case 1: return fromText(generateCode(seed, 20000), FLOW_SEQUENCE);
    case 2: case 3: case 4: case 5:
      return fromText(WORKS[source - 2].text, FLOW_SEQUENCE);
    case 6: {
      const stream = fromText(customText ?? '', FLOW_SEQUENCE);
      // Nothing typed: fall back rather than render an empty frame.
      return stream.codepoints.length ? stream : { codepoints: alphabet(set), flow: FLOW_SCATTER };
    }
    default: return { codepoints: alphabet(set), flow: FLOW_SCATTER };
  }
}

//===========================================================================
// Shaders — verbatim from source/Shaders.cpp
//===========================================================================

const VERTEX = `#version 410 core

layout( location = 0 ) in vec4 vPosition;
layout( location = 1 ) in vec2 vUV;

out vec2 uv;

void main()
{
	gl_Position = vPosition;
	uv = vUV;
}
`;

const EFFECT_DEFINE = '#define DOWNPOUR_OVER_INPUT 1\n';

const RAIN = `#version 410 core

in vec2 uv;
out vec4 fragColor;

//---------------------------------------------------------------------------
// The grid and the clock.
//---------------------------------------------------------------------------
uniform float Time;          //host clock, seconds
uniform vec2  Grid;          //columns, rows
uniform float Speed;         //rows per second, before per-column variation
uniform float Trail;         //trail length as a fraction of the run
uniform float Density;       //fraction of columns carrying a drop
uniform float Mutate;        //glyph changes per second
uniform float Falloff;       //brightness exponent along the trail
uniform uint  Seed;
uniform int   DirectionMode; //0 down, 1 up, 2 right, 3 left
uniform int   FlowMode;      //0 scatter, 1 sequence

//---------------------------------------------------------------------------
// Glyphs.
//---------------------------------------------------------------------------
uniform sampler2D AtlasTexture;  //single channel, 255 for ink
uniform sampler2D StreamTexture; //atlas slot numbers -- texelFetch only

uniform vec2 StreamSize;         //texels of StreamTexture actually in use
uniform int   StreamLength;      //entries in the stream
uniform vec2  AtlasLayout;       //slot columns, slot rows
uniform vec2  AtlasSize;         //the atlas texture's own size, in texels
uniform vec3  AtlasMetrics;      //slot size, glyph size, border, all in texels
uniform vec2  Resolution;        //output size in pixels
uniform float GlyphScale;        //glyph size within its cell, 1 = fills it
uniform float MirrorGlyphs;      //0 or 1

//---------------------------------------------------------------------------
// Colour.
//---------------------------------------------------------------------------
uniform vec3  TextColour;
uniform float TextOpacity;
uniform vec3  HeadColour;
uniform float HeadBoost;
uniform vec3  BackColour;
uniform float BackOpacity;
uniform float Glow;

#ifdef DOWNPOUR_OVER_INPUT
uniform sampler2D InputTexture;
uniform vec2 MaxUV;   //the part of the input texture that is really picture
uniform float Mix_;   //how much of the rain to lay over the clip
#endif

//===========================================================================
// The mirror of Rain.cpp starts here.
//===========================================================================

const float kSpeedMin = 0.45;      //= mirrored
const float kTrailMin = 0.35;      //= mirrored
const float kTailGap  = 0.6;       //= mirrored
const float kHeadRows = 1.5;       //= mirrored

const uint kSaltSpeed  = 0x9E3779B9u;//= mirrored
const uint kSaltPhase  = 0x85EBCA6Bu;//= mirrored
const uint kSaltTrail  = 0xC2B2AE35u;//= mirrored
const uint kSaltActive = 0x27D4EB2Fu;//= mirrored
const uint kSaltGlyph  = 0x165667B1u;//= mirrored
const uint kSaltMutate = 0xD3A2646Cu;//= mirrored

uint Hash( uint x )                                //= mirrored
{
	x ^= x >> 16;
	x *= 0x7FEB352Du;
	x ^= x >> 15;
	x *= 0x846CA68Bu;
	x ^= x >> 16;
	return x;
}

uint HashCombine( uint seed, uint v )              //= mirrored
{
	return Hash( seed ^ ( v * 0x9E3779B9u ) );
}

//The top 24 bits, which is the widest slice that converts to float without
//rounding. Using all 32 would round, and round differently on the CPU.
float Unit( uint h )                               //= mirrored
{
	return float( h >> 8 ) * ( 1.0 / 16777216.0 );
}

//===========================================================================
// Cell resolution.
//===========================================================================
struct CellResult
{
	float brightness;
	float head;
	int   stream;
};

CellResult EvaluateCell( ivec2 cellIndex )         //= mirrored
{
	CellResult result;
	result.brightness = 0.0;
	result.head       = 0.0;
	result.stream     = 0;

	int columns = int( Grid.x );
	int rows    = int( Grid.y );
	if( columns <= 0 || rows <= 0 )
		return result;

	//Direction, as a remap onto a thing that falls downwards.       //= mirrored
	int column;
	int row;
	if( DirectionMode == 1 )      { column = cellIndex.x; row = rows - 1 - cellIndex.y; }
	else if( DirectionMode == 2 ) { column = cellIndex.y; row = cellIndex.x; }
	else if( DirectionMode == 3 ) { column = cellIndex.y; row = columns - 1 - cellIndex.x; }
	else                          { column = cellIndex.x; row = cellIndex.y; }

	bool horizontal = ( DirectionMode == 2 || DirectionMode == 3 );
	float run = horizontal ? float( columns ) : float( rows );

	uint columnSeed = HashCombine( Seed, uint( column ) );           //= mirrored

	float speedScale = mix( kSpeedMin, 1.0, Unit( HashCombine( columnSeed, kSaltSpeed ) ) );
	float phase      = Unit( HashCombine( columnSeed, kSaltPhase ) );

	float cycle  = run * ( 1.0 + kTailGap );                         //= mirrored
	float travel = Time * Speed * speedScale + phase * cycle;        //= mirrored
	float drop   = floor( travel / cycle );                          //= mirrored
	float head   = travel - cycle * drop;                            //= mirrored

	//A column that loses the density draw carries no drop at all.   //= mirrored
	float activeDraw = Unit( HashCombine( columnSeed, kSaltActive ) );
	if( activeDraw >= Density )
		return result;

	float trailScale = mix( kTrailMin, 1.0, Unit( HashCombine( columnSeed, kSaltTrail ) ) );
	float trailRows  = max( 1.0, Trail * trailScale * run );         //= mirrored

	float distance = head - float( row );                            //= mirrored

	//Hard leading edge, soft tail -- which is what a phosphor does.  //= mirrored
	if( distance < 0.0 || distance > trailRows )
		return result;

	float along = distance / trailRows;
	result.brightness = pow( clamp( 1.0 - along, 0.0, 1.0 ), Falloff );
	result.head       = 1.0 - smoothstep( 0.0, kHeadRows, distance );

	if( FlowMode == 1 )                                              //= mirrored
	{
		//Consecutive down a column, consecutive across columns, advancing by a
		//screenful per recycle, so a document plays out in order.
		float perScreen = float( columns ) * run;
		float position  = float( column ) * run + drop * perScreen + float( row );
		float wrapped   = position - float( StreamLength ) * floor( position / float( StreamLength ) );
		result.stream   = int( wrapped );
	}
	else
	{
		uint cellSeed = HashCombine( columnSeed, uint( row ) * 0x9E3779B9u + kSaltGlyph );

		if( Mutate > 0.0 )
		{
			float mutatePhase = Unit( HashCombine( cellSeed, kSaltMutate ) );
			int tick = int( floor( Time * Mutate + mutatePhase ) );
			cellSeed = HashCombine( cellSeed, uint( tick ) );
		}
		else
		{
			cellSeed = HashCombine( cellSeed, uint( int( drop ) ) );
		}

		result.stream = StreamLength > 0 ? int( Hash( cellSeed ) % uint( StreamLength ) ) : 0;
	}

	return result;
}

//===========================================================================
// End of the mirror.
//===========================================================================

///Atlas slot for a stream position. texelFetch, never texture: these are slot
///numbers, and an interpolated slot number addresses a glyph nobody chose.
int SlotForStream( int position )
{
	int stride = int( StreamSize.x );
	if( stride <= 0 )
		return 0;
	ivec2 texel = ivec2( position % stride, position / stride );
	return int( texelFetch( StreamTexture, texel, 0 ).r + 0.5 );
}

///Ink coverage at \`local\` (0..1 within the glyph body) for one atlas slot.
float SampleGlyph( int slot, vec2 local )
{
	if( any( lessThan( local, vec2( 0.0 ) ) ) || any( greaterThan( local, vec2( 1.0 ) ) ) )
		return 0.0;

	float slotPx   = AtlasMetrics.x;
	float glyphPx  = AtlasMetrics.y;
	float borderPx = AtlasMetrics.z;

	vec2 slotXY = vec2( float( slot % int( AtlasLayout.x ) ),
	                    float( slot / int( AtlasLayout.x ) ) );

	//**This is where the two vertical conventions meet, and it is the only
	//place either of them may change.**
	//
	//\`local\` comes from the cell grid, whose origin is top left and whose y runs
	//*down* the screen -- that is how falling rain is described and how Rain.cpp
	//is written. A GL texture's v runs *up*. Without this flip every glyph
	//renders upside down, which is nearly invisible on mirrored katakana, reads
	//as merely an odd typeface on lower case, and was caught only by
	//\`--readback\` scoring 34% against an atlas it should have matched exactly.
	vec2 upright = vec2( local.x, 1.0 - local.y );

	//Half a texel in on every side. GL_LINEAR at the very edge of the glyph
	//body takes half its weight from the border, and the border is blank -- so
	//without this every glyph loses its outermost row of ink to a fade.
	vec2 inset = vec2( 0.5 / glyphPx );
	vec2 body  = mix( inset, vec2( 1.0 ) - inset, upright );

	vec2 texel = slotXY * slotPx + vec2( borderPx ) + body * glyphPx;

	//The texture's own size, not the part of it the slots cover. 31 slots of 66
	//texels is 2046 of the 2048, and dividing by 2046 puts every glyph a tenth
	//of a percent off -- which is invisible in the middle of a stroke and shows
	//up as a sliver of the next slot along the far edge.
	return texture( AtlasTexture, texel / AtlasSize ).r;
}

void main()
{
	int columns = int( Grid.x );
	int rows    = int( Grid.y );

	//---------------------------------------------------------------------
	//Which cell, and where in it. uv has its origin bottom-left; the grid has
	//its origin top-left, to match Rain.cpp and to match the way anyone
	//describes falling rain. The flip happens here and nowhere else.
	//---------------------------------------------------------------------
	vec2 gridUV = vec2( uv.x, 1.0 - uv.y ) * Grid;
	ivec2 cellIndex = ivec2( floor( gridUV ) );
	cellIndex = clamp( cellIndex, ivec2( 0 ), ivec2( columns - 1, rows - 1 ) );

	vec2 within = gridUV - vec2( cellIndex );

	CellResult cell = EvaluateCell( cellIndex );

	//---------------------------------------------------------------------
	//Draw the glyph. The atlas is square, so the glyph is fitted into the
	//short axis of the cell and centred on the long one -- stretching it to
	//the cell's aspect is the difference between a font and a smeared font,
	//and cells are rarely square.
	//---------------------------------------------------------------------
	float ink = 0.0;
	if( cell.brightness > 0.0 )
	{
		//In **pixels**, not in frame units. A cell that is square on screen is
		//not square in 0..1 coordinates on anything but a square output, so
		//fitting the glyph in normalised space squashed every glyph by the
		//frame's aspect ratio -- 16:9 output, glyphs 56% of their proper height.
		vec2 cellPx = Resolution / Grid;
		float shortSide = min( cellPx.x, cellPx.y );
		vec2 span = vec2( shortSide ) / cellPx;        //square, as a fraction of the cell
		span *= max( GlyphScale, 0.001 );

		vec2 local = ( within - 0.5 * ( vec2( 1.0 ) - span ) ) / span;

		//The film's glyphs are mirrored. Doing it here rather than in the atlas
		//keeps GlyphData.cpp readable against a real font, which is what makes
		//a badly drawn glyph findable.
		local.x = mix( local.x, 1.0 - local.x, MirrorGlyphs );

		int slot = SlotForStream( cell.stream );
		ink = SampleGlyph( slot, local );
	}

	//---------------------------------------------------------------------
	//Composite.
	//---------------------------------------------------------------------
	vec3 lit = mix( TextColour, HeadColour, cell.head * HeadBoost );

	//Glow is a lift on the trail's own brightness rather than a blur: a blur
	//would need a second pass and a buffer, and at rain speed what reads as
	//glow is mostly the trail refusing to go fully dark.
	float body = cell.brightness + Glow * cell.brightness * ( 1.0 - cell.brightness );

	float textAlpha = clamp( ink * body * TextOpacity, 0.0, 1.0 );
	vec3  textRGB   = lit * textAlpha;   //premultiplied

	vec4 back = vec4( BackColour * BackOpacity, BackOpacity );

#ifdef DOWNPOUR_OVER_INPUT
	//The clip sits under our own background, which sits under the rain. So
	//Background Opacity is how much the plugin veils the footage, and the two
	//controls keep the meanings they have in the source plugin.
	vec2 picture = clamp( uv, vec2( 0.0 ), vec2( 1.0 ) );
	vec4 clip = texture( InputTexture, picture * MaxUV );

	vec4 veiled = back + clip * ( 1.0 - back.a );
	vec4 rained = vec4( textRGB, textAlpha ) + veiled * ( 1.0 - textAlpha );

	fragColor = mix( clip, rained, clamp( Mix_, 0.0, 1.0 ) );
#else
	fragColor = vec4( textRGB, textAlpha ) + back * ( 1.0 - textAlpha );
#endif
}
`;

/// One shader, compiled with or without the input path — which is exactly how
/// the two bundles are built: a constructor flag and a #define handed to the
/// shader compiler, from one `downpour_core` object library.
function withDefines(source, defines) {
  const afterVersion = source.indexOf('\n');
  return source.slice(0, afterVersion + 1) + defines + source.slice(afterVersion + 1);
}

//===========================================================================
// The renderer — a port of Render() in source/Downpour.cpp
//===========================================================================

function createRenderer(gl, quad) {
  const sourceProgram = new Program(gl, VERTEX, RAIN, 'downpour source');
  const effectProgram = new Program(gl, VERTEX, withDefines(RAIN, EFFECT_DEFINE), 'downpour over');

  // ---- the atlas --------------------------------------------------------
  const atlasTexture = gl.createTexture();
  gl.bindTexture(gl.TEXTURE_2D, atlasTexture);
  gl.pixelStorei(gl.UNPACK_ALIGNMENT, 1);
  gl.texImage2D(gl.TEXTURE_2D, 0, gl.R8, ATLAS_PX, ATLAS_PX, 0, gl.RED, gl.UNSIGNED_BYTE, buildBuiltinAtlas());
  gl.pixelStorei(gl.UNPACK_ALIGNMENT, 4);
  gl.generateMipmap(gl.TEXTURE_2D);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR_MIPMAP_LINEAR);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
  gl.bindTexture(gl.TEXTURE_2D, null);

  // ---- the stream -------------------------------------------------------
  const streamTexture = gl.createTexture();
  gl.bindTexture(gl.TEXTURE_2D, streamTexture);
  // NEAREST and no mipmaps, and it is read with texelFetch anyway. These values
  // are slot numbers, not quantities: interpolate two of them and you address a
  // third glyph that neither cell asked for.
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
  gl.bindTexture(gl.TEXTURE_2D, null);

  let contentKey = null;
  let streamWidth = 1;
  let streamHeight = 1;
  let streamLength = 1;
  let flow = FLOW_SCATTER;

  function rebuildContent(source, set, customText, seed) {
    const key = `${source} ${set} ${customText} ${source === 1 ? seed : ''}`;
    if (key === contentKey) return;
    contentKey = key;

    const stream = buildStream(source, set, customText, seed);
    flow = stream.flow;

    // Fallback is **per codepoint**, not per font: an unknown character resolves
    // to the blank slot rather than taking the whole document with it.
    const slots = stream.codepoints.map((cp) => BUILTIN_SLOT.get(cp) ?? BLANK_SLOT);
    if (!slots.length) slots.push(BLANK_SLOT);

    streamLength = slots.length;
    streamWidth = Math.min(STREAM_WIDTH, slots.length);
    streamHeight = Math.ceil(slots.length / streamWidth);

    const data = new Float32Array(streamWidth * streamHeight).fill(BLANK_SLOT);
    slots.forEach((slot, i) => { data[i] = slot; });

    gl.bindTexture(gl.TEXTURE_2D, streamTexture);
    gl.pixelStorei(gl.UNPACK_ALIGNMENT, 1);
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.R32F, streamWidth, streamHeight, 0, gl.RED, gl.FLOAT, data);
    gl.pixelStorei(gl.UNPACK_ALIGNMENT, 4);
    gl.bindTexture(gl.TEXTURE_2D, null);
  }

  return {
    render({ input, params, width, height, time, variant }) {
      const source = Math.round(params.get('source'));
      const seed = seedFromParam(params.get('seed'));
      rebuildContent(source, Math.round(params.get('charset')), params.get('customText'), seed);

      const overInput = variant === 'over';
      const shader = (overInput ? effectProgram : sourceProgram).use();

      const columns = columnsFromParam(params.get('columns'));
      const rows = rowsForAspect(columns, width, height);

      bindTexture(gl, 0, atlasTexture);
      bindTexture(gl, 1, streamTexture);
      shader.setSampler('AtlasTexture', 0);
      shader.setSampler('StreamTexture', 1);

      shader.set('Time', time);
      shader.set('Grid', columns, rows);
      shader.set('Speed', speedFromParam(params.get('speed')));
      shader.set('Trail', trailFromParam(params.get('trail')));
      shader.set('Density', params.get('density'));
      shader.set('Mutate', mutateFromParam(params.get('mutate')));
      shader.set('Falloff', falloffFromParam(params.get('falloff')));
      shader.setInt('DirectionMode', Math.round(params.get('direction')));
      shader.setInt('FlowMode', flow);
      shader.setInt('StreamLength', streamLength);
      shader.set('StreamSize', streamWidth, streamHeight);
      shader.set('Resolution', width, height);
      shader.setUint('Seed', seed);

      shader.set('AtlasLayout', ATLAS_COLS, ATLAS_ROWS);
      shader.set('AtlasSize', ATLAS_PX, ATLAS_PX);
      shader.set('AtlasMetrics', SLOT_PX, GLYPH_PX, BORDER_PX);
      shader.set('GlyphScale', glyphScaleFromParam(params.get('glyphScale')));
      shader.set('MirrorGlyphs', params.get('mirror') >= 0.5 ? 1 : 0);

      shader.set('TextColour', params.get('textR'), params.get('textG'), params.get('textB'));
      shader.set('TextOpacity', params.get('textOpacity'));
      shader.set('HeadColour', params.get('headR'), params.get('headG'), params.get('headB'));
      shader.set('HeadBoost', headBoostFromParam(params.get('headBoost')));
      shader.set('BackColour', params.get('backR'), params.get('backG'), params.get('backB'));
      shader.set('BackOpacity', params.get('backOpacity'));
      shader.set('Glow', glowFromParam(params.get('glow')));

      if (overInput) {
        bindTexture(gl, 2, input.texture);
        shader.setSampler('InputTexture', 2);
        shader.set('MaxUV', 1, 1);
        shader.set('Mix_', params.get('mix'));
      }

      quad.draw();

      bindTexture(gl, 2, null);
      bindTexture(gl, 1, null);
      bindTexture(gl, 0, null);
    },
  };
}

//===========================================================================

const pct = (v) => `${Math.round(v * 100)}%`;

mountDemo({
  name: 'Downpour',
  pluginId: 'DP01 · DP02',
  tagline:
    'Digital rain: columns of characters falling at their own speeds, each with a bright leading glyph and a trail fading out behind it. Every cell is a closed-form function of its column, its row and the clock — no simulation state, so the rain cannot slow down when the host’s frame rate does.',
  repo: 'https://github.com/stoatworks-labs/downpour',
  page: 'https://stoatworks-labs.com/software/downpour/',
  video: 'https://www.youtube.com/watch?v=Sdvmpz_GiTo',

  showBackdrop: true,

  variants: {
    label: 'Plugin',
    default: 'source',
    options: [
      { id: 'source', name: 'Downpour (source)', hint: 'FF_SOURCE — draws over its own background, takes no input.' },
      { id: 'over', name: 'Downpour Over (effect)', hint: 'FF_EFFECT — draws the same rain over the clip it sits on.' },
    ],
  },

  params: [
    {
      id: 'speed', name: 'Speed', type: 'standard', default: 0.52, group: 'Rain',
      display: (v) => `${speedFromParam(v).toFixed(1)} rows/s`,
    },
    {
      id: 'direction', name: 'Direction', type: 'option', default: 0, group: 'Rain',
      elements: ['Down', 'Up', 'Right', 'Left'],
    },
    {
      id: 'columns', name: 'Columns', type: 'standard', default: 0.59, group: 'Rain',
      display: (v) => `${columnsFromParam(v)}`,
    },
    {
      id: 'trail', name: 'Trail', type: 'standard', default: 0.43, group: 'Rain',
      display: (v) => pct(trailFromParam(v)),
      hint: 'Trail length as a fraction of the run, before the per-column variation.',
    },
    {
      id: 'density', name: 'Density', type: 'standard', default: 0.85, group: 'Rain',
      display: pct,
      hint: 'Fraction of columns carrying a drop. Turning it down empties columns rather than dimming all of them.',
    },
    {
      id: 'mutate', name: 'Mutation', type: 'standard', default: 0.55, group: 'Rain',
      display: (v) => (mutateFromParam(v) === 0 ? 'off' : `${mutateFromParam(v).toFixed(1)}/s`),
      hint: 'Below a twentieth of the travel this is exactly zero — a dead zone, because 0.1 changes a second reads as a bug rather than as slow.',
    },
    {
      id: 'falloff', name: 'Fade', type: 'standard', default: 0.62, group: 'Rain',
      display: (v) => `γ ${falloffFromParam(v).toFixed(2)}`,
    },
    {
      id: 'seed', name: 'Seed', type: 'standard', default: 0.0, group: 'Rain',
      display: (v) => `${seedFromParam(v)}`,
      hint: 'Quantised on purpose: the seed picks a rain, not a position in a continuum.',
    },

    {
      id: 'source', name: 'Source', type: 'option', default: 0, group: 'Text',
      elements: SOURCE_NAMES,
      hint: 'A loaded document plays out in order rather than being sampled from.',
    },
    {
      id: 'charset', name: 'Characters', type: 'option', default: 1, group: 'Text',
      elements: CHARSET_NAMES,
      hint: 'Only read when Source is Junk.',
    },
    {
      id: 'customText', name: 'Custom Text', type: 'text', default: 'Wake up...', group: 'Text',
      placeholder: 'Wake up...',
    },
    {
      id: 'mirror', name: 'Mirror Glyphs', type: 'boolean', default: 1, group: 'Text',
      hint: 'The film’s glyphs are mirrored. Flipped at sample time, so the atlas stays checkable against a real font.',
    },

    {
      id: 'glyphScale', name: 'Glyph Size', type: 'standard', default: 0.75, group: 'Font',
      display: (v) => `${glyphScaleFromParam(v).toFixed(2)}×`,
      hint: 'Two straight segments meeting at exactly 1.0 at three quarters of the travel.',
    },

    { id: 'textR', name: 'Text', type: 'colour', default: 0.3, group: 'Colour' },
    { id: 'textG', name: 'Text_Green', type: 'colour', default: 1.0, group: 'Colour' },
    { id: 'textB', name: 'Text_Blue', type: 'colour', default: 0.42, group: 'Colour' },
    { id: 'textOpacity', name: 'Text Opacity', type: 'standard', default: 1.0, group: 'Colour', display: pct },
    { id: 'headR', name: 'Head', type: 'colour', default: 0.82, group: 'Colour' },
    { id: 'headG', name: 'Head_Green', type: 'colour', default: 1.0, group: 'Colour' },
    { id: 'headB', name: 'Head_Blue', type: 'colour', default: 0.88, group: 'Colour' },
    { id: 'headBoost', name: 'Head Boost', type: 'standard', default: 1.0, group: 'Colour', display: pct },
    { id: 'backR', name: 'Background', type: 'colour', default: 0.0, group: 'Colour' },
    { id: 'backG', name: 'Background_Green', type: 'colour', default: 0.04, group: 'Colour' },
    { id: 'backB', name: 'Background_Blue', type: 'colour', default: 0.01, group: 'Colour' },
    { id: 'backOpacity', name: 'Background Opacity', type: 'standard', default: 1.0, group: 'Colour', display: pct },
    {
      id: 'glow', name: 'Glow', type: 'standard', default: 0.25, group: 'Colour',
      display: (v) => `${glowFromParam(v).toFixed(2)}×`,
      hint: 'A lift on the trail’s own brightness rather than a blur — at rain speed what reads as glow is mostly the trail refusing to go fully dark.',
    },

    {
      id: 'mix', name: 'Mix', type: 'standard', default: 1.0, group: 'Output', display: pct,
      hint: 'Only read by the effect bundle: how much of the rain to lay over the clip.',
    },
  ],

  sources: ['scene', 'spot', 'detail', 'grid', 'bars'],

  presets: {
    'Code rain (defaults)': {},
    'Alice, falling in order': { source: 2, columns: 0.5, mutate: 0, speed: 0.42, trail: 0.6 },
    'Generated code': { source: 1, columns: 0.45, mutate: 0.1, textR: 0.5, textG: 0.9, textB: 1 },
    'Sparse and slow': { density: 0.35, speed: 0.3, trail: 0.7, glow: 0.5, columns: 0.42 },
    'Sideways': { direction: 2, speed: 0.6, columns: 0.5 },
    'Amber terminal': {
      textR: 1, textG: 0.72, textB: 0.2, headR: 1, headG: 0.95, headB: 0.8,
      backR: 0.04, backG: 0.02, backB: 0.0, charset: 5,
    },
    'Over the clip': { backOpacity: 0.45, mix: 1, density: 0.6 },
  },

  differences: [
    'Both bundles are here. The plugin ships two — `Downpour` (a source, with its own background) and `Downpour Over` (an effect, over the clip) — from one class, differing by a constructor flag and a #define handed to the shader compiler. The Plugin picker in the transport bar compiles this page’s copy the same two ways.',
    'The plugin’s Font control lists every typeface installed on the machine, found by walking the OS font directories and reading each file’s `name` table, with a per-codepoint fallback to the drawn face. A browser cannot enumerate installed fonts, so this page offers the built-in face only — the Font dropdown, Font File picker and the TrueType rasteriser are all absent rather than approximated.',
    'The Text File source is likewise absent: it opens a path with a file picker, and there is nothing here for a path to mean. Custom Text does the same job for a short string.',
    'The four passages are read from the repository’s own `texts/` directory by demo/tools/extract_data.py — the same files `tools/embed_texts.py` generates `Corpus.cpp` from. They are excerpts, not complete works.',
    'The plugin’s own proof is `dptest --readback`, which renders Alice and reads the characters back out of the pixels demanding 100%, plus `--rain` comparing the shader against the C++ mirror over ~40k cells. Both live in the repository; nothing on this page measures anything.',
  ],

  createRenderer,
});
