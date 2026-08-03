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

step "generated files are in sync"
check python3 tools/embed_texts.py --check
check python3 tools/extract_texts.py --check

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
	if ! nm -gU "$binary" 2>/dev/null | grep -q '_plugMain'; then
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
