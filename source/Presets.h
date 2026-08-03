#pragma once

/**
    Factory presets: named rains an operator can reach in one gesture. Each
    entry is a recognisable *weather* — the film's green code, a binary
    squall, a page of Plato dissolving down the screen — not a random
    collection of slider positions.

    The values live in the same 0..1 parameter space both builds expose (the
    FFGL and OFX builds deliberately share it), so ONE table drives both and a
    preset looks identical in Resolume and Resolve. Plain data only; the
    application machinery lives with each host's glue. Both FFGL plugins (the
    source and the effect) share DownpourPlugin, so both get the dropdown
    from this one table too.

    Element 0 of the host-facing dropdown is "Custom" and is not in this
    table: it means "the sliders are the truth".

    A preset covers the rain, text-selection and colour parameters. It stays
    off the operator's own material and machine: Custom Text, Text File and
    the Font choice (a system font index means something different on every
    machine), the Seed (which rain, not what kind of rain), and Mix (how much
    effect is wanted is not part of any look).
*/

namespace downpour
{
namespace presets
{
/// The parameters a preset sets, in one fixed order. The FFGL build binds
/// this order to its ParamIDs and the OFX build to its param handles; both
/// static_assert against kParamCount so the three lists cannot drift apart
/// silently.
enum Param
{
	kSpeed,
	kDirection,
	kColumns,
	kTrail,
	kDensity,
	kMutate,
	kFalloff,
	kSource,
	kCharSet,
	kMirror,
	kGlyphScale,
	kTextR,
	kTextG,
	kTextB,
	kTextOpacity,
	kHeadR,
	kHeadG,
	kHeadB,
	kHeadBoost,
	kBackR,
	kBackG,
	kBackB,
	kBackOpacity,
	kGlow,
	kParamCount
};

struct Preset
{
	const char* name;
	float v[ kParamCount ];
};

// Option values are element indices: Direction 0 Down / 1 Up / 2 Right /
// 3 Left; Source 0 Junk / 1 Code / 2 Alice / 3 Republic / 4 Hamlet /
// 5 Discourse; Characters 0 Katakana / 1 Katakana+Digits / 2 Digits / 3 Hex /
// 4 Binary / 5 ASCII / 6 Latin. Speed/Columns/Trail and friends are the
// curves in Controls.cpp (0.52 is ~8 rows/s, 0.59 is ~60 columns).
inline constexpr Preset kPresets[] = {
	// The code rain as shipped: mirrored katakana and digits in phosphor
	// green. The plugin's own defaults, named so they stay reachable.
	{ "The Film",
	  { /*Speed*/ 0.52f, /*Dir*/ 0, /*Cols*/ 0.59f, /*Trail*/ 0.43f, /*Density*/ 0.85f, /*Mutate*/ 0.55f,
	    /*Falloff*/ 0.62f, /*Source*/ 0, /*Chars*/ 1, /*Mirror*/ 1.0f, /*Scale*/ 0.75f,
	    /*Text*/ 0.30f, 1.00f, 0.42f, /*TextOp*/ 1.0f, /*Head*/ 0.82f, 1.00f, 0.88f, /*Boost*/ 1.0f,
	    /*Back*/ 0.00f, 0.04f, 0.01f, /*BackOp*/ 1.0f, /*Glow*/ 0.25f } },

	// A hex dump scrolling on an amber terminal: unhurried, unmirrored, warm.
	{ "Amber Dump",
	  { /*Speed*/ 0.45f, /*Dir*/ 0, /*Cols*/ 0.55f, /*Trail*/ 0.5f, /*Density*/ 0.8f, /*Mutate*/ 0.35f,
	    /*Falloff*/ 0.6f, /*Source*/ 0, /*Chars*/ 3, /*Mirror*/ 0.0f, /*Scale*/ 0.75f,
	    /*Text*/ 1.00f, 0.72f, 0.20f, /*TextOp*/ 1.0f, /*Head*/ 1.00f, 0.90f, 0.60f, /*Boost*/ 0.8f,
	    /*Back*/ 0.05f, 0.03f, 0.00f, /*BackOp*/ 1.0f, /*Glow*/ 0.3f } },

	// Ones and zeros in a hurry: dense columns, short trails, blue-white.
	{ "Binary Storm",
	  { /*Speed*/ 0.7f, /*Dir*/ 0, /*Cols*/ 0.75f, /*Trail*/ 0.3f, /*Density*/ 0.95f, /*Mutate*/ 0.6f,
	    /*Falloff*/ 0.55f, /*Source*/ 0, /*Chars*/ 4, /*Mirror*/ 0.0f, /*Scale*/ 0.7f,
	    /*Text*/ 0.40f, 0.80f, 1.00f, /*TextOp*/ 1.0f, /*Head*/ 0.95f, 1.00f, 1.00f, /*Boost*/ 1.0f,
	    /*Back*/ 0.00f, 0.02f, 0.06f, /*BackOp*/ 1.0f, /*Glow*/ 0.4f } },

	// Generated source code drifting sideways, the way a scroller reads.
	{ "Code Crawl",
	  { /*Speed*/ 0.48f, /*Dir*/ 3, /*Cols*/ 0.5f, /*Trail*/ 0.55f, /*Density*/ 0.75f, /*Mutate*/ 0.2f,
	    /*Falloff*/ 0.6f, /*Source*/ 1, /*Chars*/ 5, /*Mirror*/ 0.0f, /*Scale*/ 0.75f,
	    /*Text*/ 0.50f, 0.90f, 0.90f, /*TextOp*/ 1.0f, /*Head*/ 0.85f, 1.00f, 1.00f, /*Boost*/ 0.7f,
	    /*Back*/ 0.00f, 0.02f, 0.06f, /*BackOp*/ 1.0f, /*Glow*/ 0.2f } },

	// The allegory of the cave, falling slowly with long trails: shadows of
	// text rather than text.
	{ "Plato's Cave",
	  { /*Speed*/ 0.35f, /*Dir*/ 0, /*Cols*/ 0.5f, /*Trail*/ 0.8f, /*Density*/ 0.7f, /*Mutate*/ 0.1f,
	    /*Falloff*/ 0.75f, /*Source*/ 3, /*Chars*/ 6, /*Mirror*/ 0.0f, /*Scale*/ 0.75f,
	    /*Text*/ 0.25f, 0.75f, 0.35f, /*TextOp*/ 0.9f, /*Head*/ 0.70f, 0.95f, 0.75f, /*Boost*/ 0.6f,
	    /*Back*/ 0.00f, 0.03f, 0.01f, /*BackOp*/ 1.0f, /*Glow*/ 0.35f } },

	// Alice, upward, in white on black: type set loose from the page.
	{ "White Rabbit",
	  { /*Speed*/ 0.4f, /*Dir*/ 1, /*Cols*/ 0.45f, /*Trail*/ 0.6f, /*Density*/ 0.7f, /*Mutate*/ 0.05f,
	    /*Falloff*/ 0.65f, /*Source*/ 2, /*Chars*/ 6, /*Mirror*/ 0.0f, /*Scale*/ 0.75f,
	    /*Text*/ 0.92f, 0.92f, 0.92f, /*TextOp*/ 1.0f, /*Head*/ 1.00f, 1.00f, 1.00f, /*Boost*/ 0.5f,
	    /*Back*/ 0.02f, 0.02f, 0.02f, /*BackOp*/ 1.0f, /*Glow*/ 0.1f } },

	// The film's rain gone red: same weather, other pill.
	{ "Red Pill",
	  { /*Speed*/ 0.52f, /*Dir*/ 0, /*Cols*/ 0.59f, /*Trail*/ 0.43f, /*Density*/ 0.85f, /*Mutate*/ 0.55f,
	    /*Falloff*/ 0.62f, /*Source*/ 0, /*Chars*/ 1, /*Mirror*/ 1.0f, /*Scale*/ 0.75f,
	    /*Text*/ 1.00f, 0.25f, 0.20f, /*TextOp*/ 1.0f, /*Head*/ 1.00f, 0.70f, 0.60f, /*Boost*/ 1.0f,
	    /*Back*/ 0.04f, 0.00f, 0.00f, /*BackOp*/ 1.0f, /*Glow*/ 0.35f } },
};

inline constexpr int kCount = int( sizeof( kPresets ) / sizeof( kPresets[ 0 ] ) );

} // namespace presets
} // namespace downpour
