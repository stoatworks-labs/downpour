# Attributions

Downpour is built on other people's work. This file lists what that work is, who did
it, and what it is doing here.

It is generated — the master lists live in the `stoatworks-backend` repo and are
pushed out by `scripts/sync-attributions.py`. Edit it there, not here.

## Code we derived from other people's work

Someone else solved this first, and this project would not exist in its current form without their work.

### Four public-domain works, via Project Gutenberg

<https://www.gutenberg.org>  
Licence: public domain in the United States

The built-in passages are body-only excerpts of Alice's Adventures in Wonderland (Lewis Carroll, 1865), The Republic Book VII (Plato, tr. Benjamin Jowett, 1871), Discourse on the Method Part IV (Rene Descartes, tr. John Veitch, 1850) and Hamlet Act III Scene I (Shakespeare, c. 1600). The Gutenberg header, footer and licence are deliberately never included, and the extractor asserts that no reference to them survives — so the files carry the public-domain work and nothing of Gutenberg's own added matter.

## Third-party code this project uses

Libraries, SDKs and frameworks the project is built on or bundles.

### Resolume FFGL SDK

<https://github.com/resolume/ffgl>  
Licence: BSD-3-Clause  
Copyright: FreeFrame

Vendored as a git submodule at external/ffgl (third_party/ffgl in oxbow).

The plugin ABI itself. An FFGL effect or source is defined by this SDK's headers — there is no other way to be loadable by Resolume Arena and Avenue.

### GLEW — the OpenGL Extension Wrangler Library

<https://github.com/nigels-com/glew>  
Licence: BSD-3-Clause (with Mesa 3-D and Khronos components)  
Copyright: Milan Ikits, Marcelo E. Magallon and Lev Povalahev

Arrives inside the FFGL submodule at external/ffgl/deps/glew-2.1.0. Not fetched separately.

Resolves OpenGL entry points on Windows, where the system headers stop at OpenGL 1.1.

### libpng

<http://www.libpng.org/pub/png/libpng.html>  
Licence: PNG Reference Library License (libpng)  
Copyright: the PNG Reference Library authors

Arrives inside the FFGL submodule, under the SDK's CustomThumbnail sample.

Part of the upstream SDK tree rather than something these plugins call directly — listed because it is present in the checkout.

### OpenFX image effect plug-in API

<https://github.com/AcademySoftwareFoundation/openfx>  
Licence: BSD-3-Clause  
Copyright: OpenFX and contributors to the OpenFX project

Vendored at external/openfx — a git submodule in resolume-ofx-bridge, a copy of the headers and Support library elsewhere.

The plugin ABI for the DaVinci Resolve and Nuke side of the same effects, so one core renders through both hosts.

### stb_truetype

<https://github.com/nothings/stb>  
Licence: MIT or Public Domain (Unlicense), at your choice  
Copyright: Sean Barrett

Single header vendored at external/stb/stb_truetype.h.

Rasterises glyphs into the atlas the plugin samples. A whole font stack would be a large dependency for one job this header already does.

## Getting this wrong

If your work is here and the description is inaccurate, the licence is wrong, or you would rather not be listed — open an issue and it will be fixed.
