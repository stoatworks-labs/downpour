#!/usr/bin/env bash
#
# Everything, in the order that fails fastest.
#
# Each check answers a question none of the others can:
#
#   generated files   does source/Corpus.cpp still match texts/*.txt
#   passages          are the four excerpts free of Gutenberg boilerplate
#   --font            is the built-in face well formed, and where did the
#                     glyphs come from
#   --rain            does the shader's arithmetic match Rain.cpp
#   --readback        does a document survive decode, atlas, texture and shader
#   sweep.py          does every control actually reach the picture
#   lipo              is the macOS build really universal
#
set -uo pipefail

cd "$(dirname "$0")/.."

BUILD="${BUILD:-build}"
failures=0

step() {
	printf '\n\033[1m== %s\033[0m\n' "$1"
}

check() {
	if "$@"; then
		return 0
	fi
	printf '\033[31mFAILED: %s\033[0m\n' "$*"
	failures=$((failures + 1))
}

if [ ! -x "$BUILD/dptest" ]; then
	echo "$BUILD/dptest not found."
	echo "Configure with -DDOWNPOUR_BUILD_TOOLS=ON and build first:"
	echo "  cmake -B $BUILD -DCMAKE_BUILD_TYPE=Release && cmake --build $BUILD"
	exit 1
fi

#---------------------------------------------------------------------------
# Every shader, through a real GLSL compiler, before a host has to find out.
#
# A shader that will not compile presents to an operator as "the effect does
# nothing", with the real message buried in the diagnostics log -- so without
# this it is caught at run time, in a host, or not at all.
#
# --target-env=opengl4.5 with -fauto-map-locations: glslc targets SPIR-V, which
# demands an explicit layout( location ) on every uniform and varying. Those are
# Vulkan rules and not GLSL ones, and without the flag every shader "fails" for
# reasons that have nothing to do with the code.
#
# glslc is optional -- `brew install shaderc` -- so a machine without it skips
# rather than fails.
#---------------------------------------------------------------------------
shaders_compile() {
	local dir bad=0 n=0 shader

	if ! command -v glslc >/dev/null 2>&1; then
		printf '   skipped: glslc not installed (brew install shaderc)\n'
		return 0
	fi

	dir="$( mktemp -d )"

	python3 - "$dir" <<'SHADERS_PY'
import re, sys, pathlib
out = pathlib.Path( sys.argv[ 1 ] )

# Where this repo keeps its GLSL.
FILES = [
	"source/Shaders.cpp",
]

# The builds Downpour.cpp splices together: over an input, cell debug, or both.
VARIANTS = {
	"kRainShader": [
		( "effect", "#define DOWNPOUR_OVER_INPUT 1\n" ),
		( "debug", "#define DOWNPOUR_DEBUG_CELLS 1\n" ),
		( "effect_debug", "#define DOWNPOUR_OVER_INPUT 1\n#define DOWNPOUR_DEBUG_CELLS 1\n" ),
	],
}

named, unnamed = {}, []
for f in FILES:
	text = pathlib.Path( f ).read_text()
	for m in re.finditer( r'(?:(\w+)\s*(?:\[\s*\])?\s*=\s*)?R"\((.*?)\)"', text, re.S ):
		if m.group( 1 ): named[ m.group( 1 ) ] = m.group( 2 )
		else:            unnamed.append( m.group( 2 ) )
	for m in re.finditer( r'(\w+)\s*=\s*((?:"(?:[^"\\\n]|\\.)*"\s*)+);', text ):
		named.setdefault( m.group( 1 ), "".join(
			s.encode().decode( "unicode_escape" )
			for s in re.findall( r'"((?:[^"\\\n]|\\.)*)"', m.group( 2 ) ) ) )

def emit( name, body ):
	# The vertex shader is the one that writes gl_Position; everything else is a
	# fragment shader. glslc takes the stage from the extension.
	ext = ".vert" if re.search( r"\bgl_Position\s*=", body ) else ".frag"
	( out / ( name + ext ) ).write_text( body )

for name, body in named.items():
	if body.lstrip().startswith( "#version" ) and "void main" in body:
		emit( name, body )
		for label, defines in VARIANTS.get( name, [] ):
			# The plugin splices these in after the #version line, which has to
			# stay first. Each build is a separate compile and can fail alone.
			head, rest = body.split( "\n", 1 )
			emit( name + "_" + label, head + "\n" + defines + rest )
SHADERS_PY

	for shader in "$dir"/*.vert "$dir"/*.frag; do
		[ -e "$shader" ] || continue
		n=$(( n + 1 ))
		if ! glslc --target-env=opengl4.5 -fauto-map-locations \
			   "$shader" -o /dev/null 2>"$dir/err"; then
			printf '   %s does not compile\n' "$( basename "$shader" )"
			sed "s|$dir/||; s|^|      |" "$dir/err"
			bad=$(( bad + 1 ))
		fi
	done

	if [ "$n" -eq 0 ]; then
		# No shaders at all is a FAILURE, not a pass. It means the extraction
		# above has lost track of where this repo keeps its GLSL, and a check
		# that silently looks at nothing is worse than no check.
		printf '   no shaders were extracted -- the extraction has gone stale\n'
		rm -rf "$dir"
		return 1
	fi

	if [ "$bad" -eq 0 ]; then
		printf '   %d shaders, all compile\n' "$n"
	fi
	rm -rf "$dir"
	return "$bad"
}

step "shaders: every one through a real GLSL compiler"
check shaders_compile

step "generated files are in sync"
check python3 tools/embed_texts.py --check
check python3 tools/extract_texts.py --check

step "presets: every factory preset survives every host behaviour"
# Needs no GPU, so it goes first: a machine that cannot make a GL context
# can still run it.
check "$BUILD/dptest" --speed

check "$BUILD/dptest" --presets

step "the built-in face and the atlas"
check "$BUILD/dptest" --font

step "the shader against Rain.cpp"
check "$BUILD/dptest" --rain

step "a document through the atlas and back"
check "$BUILD/dptest" --readback

step "no dead controls"
check python3 tools/sweep.py --build "$BUILD"

step "the macOS build is universal"
# Checked with lipo and never with the build log. CMake latches the architecture
# list when the first target is created, so setting CMAKE_OSX_ARCHITECTURES late
# is silently ignored -- and the build still reports success while producing an
# arm64-only bundle that an Intel Resolume will not load.
for bundle in "Downpour" "Downpour Over"; do
	binary="$BUILD/$bundle.bundle/Contents/MacOS/$bundle"
	if [ ! -f "$binary" ]; then
		echo "  $bundle: not built"
		continue
	fi

	arches=$(lipo -archs "$binary" 2>/dev/null)
	echo "  $bundle: $arches"

	case "$arches" in
	*arm64*x86_64* | *x86_64*arm64*) ;;
	*)
		echo "    (single-architecture build -- fine for development, not for release)"
		;;
	esac

	# The registration lives in a file-scope constructor that nothing references
	# by name. In a STATIC archive the linker may drop the whole translation
	# unit, giving a bundle that loads, exports plugMain and reports that it
	# contains no plugins.
	# Captured, then matched from a herestring -- never `nm ... | grep -q`.
	# Under `set -o pipefail` a `grep -q` that finds its match exits
	# immediately, the writer upstream takes SIGPIPE, and the PIPELINE
	# reports failure even though the symbol is there. It is output-size
	# dependent, so it fires on the bigger binary first and looks
	# intermittent. A herestring is not a pipeline, so nothing can SIGPIPE.
	symbols=$( nm -gU "$binary" 2>/dev/null || true )
	if ! grep -q '_plugMain' <<<"$symbols"; then
		printf '\033[31m    FAILED: %s exports no plugMain\033[0m\n' "$bundle"
		failures=$((failures + 1))
	fi
done

printf '\n'
if [ "$failures" -eq 0 ]; then
	printf '\033[32mall checks passed\033[0m\n'
else
	printf '\033[31m%d check(s) failed\033[0m\n' "$failures"
fi
exit "$failures"
