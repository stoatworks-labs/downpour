#pragma once

#include <cstddef>
#include <cstdint>

/**
    The built-in reading matter.

    Four public domain works, embedded so that the plugin is self-contained: a
    bundle that has to find a text file next to itself is a bundle that stops
    working the first time somebody drags it somewhere, and "where am I
    installed" is a question with three different answers on two platforms.

    ## Excerpts, not complete works

    Each entry is a passage of a few tens of kilobytes rather than the whole
    book, and that is a deliberate choice rather than a size compromise. At any
    legible rain speed a screenful is a few thousand characters and a full
    minute of running gets through a chapter; nobody was ever going to reach the
    end of the *Republic*. What an excerpt buys is that the passage on screen is
    the passage worth having on screen.

    ## Why these are byte arrays and not string literals

    **MSVC rejects a string literal longer than 65535 bytes.** Not a warning --
    a hard error, and one that appears only on the Windows build, which is to
    say only in CI, after the macOS build has already gone green. A `char[]` of
    comma-separated bytes has no such limit.

    `tools/embed_texts.py` generates `Corpus.cpp` from `texts/*.txt`.
    `tools/verify.sh` regenerates it and fails on a diff, so the committed file
    cannot drift from its source.
*/
namespace downpour
{
/// Which built-in work. The order is the order they appear in the host's Source
/// dropdown, and the values are serialised into compositions, so entries may be
/// appended but not reordered or removed.
enum class Work : int
{
	Alice = 0, ///< Carroll, *Alice's Adventures in Wonderland*, chapter I
	Republic,  ///< Plato, *Republic* book VII -- the allegory of the cave
	Hamlet,    ///< Shakespeare, *Hamlet* III.i

	/// Descartes, *Discourse on the Method* part IV, and **not** the
	/// *Meditations*, which is what this originally set out to be. The only
	/// English *Meditations* in the public domain is Molyneux's 1680
	/// translation, which is set in period spelling and heavy italic markup --
	/// it renders as a wall of noise once the italics are stripped, and reads
	/// as a mistake with them left in. Part IV of the *Discourse* carries the
	/// same three arguments in Veitch's clean 1850 prose: methodical doubt, the
	/// dream argument, and the cogito.
	Discourse,

	Count
};

/// The text, UTF-8, or nullptr if this build has no copy of it. `length` is
/// bytes and excludes the terminator.
const char* WorkText( Work work, size_t& length );

/// Title, for the host's dropdown.
const char* WorkTitle( Work work );

} // namespace downpour
