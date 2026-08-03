#include "Downpour.h"

/**
    The generator: rain over its own background, no input.

    **This file is listed directly in the DownpourSource target, not in
    downpour_core.** Both plugins share the class; what they do not share is the
    `CFFGLPluginInfo` below, and putting either registration in the shared
    library would register both plugins into both bundles.

    It is also why the shared library is an OBJECT library rather than a STATIC
    one. `CFFGLPluginInfo` registers itself from a file-scope constructor and
    nothing ever references it by name, so in an archive the linker is entitled
    to drop the whole translation unit -- giving a bundle that loads, exports
    `plugMain`, and reports that it contains no plugins.

        nm -gU Downpour.bundle/Contents/MacOS/Downpour | grep plugMain
*/
namespace
{
class DownpourSource : public downpour::DownpourPlugin
{
public:
	DownpourSource() :
		DownpourPlugin( false )
	{
	}
};
} // namespace

static CFFGLPluginInfo PluginInfo(
	PluginFactory< DownpourSource >,                  // Create method
	"DP01",                                           // Plugin unique ID of maximum length 4
	"Downpour",                                       // Plugin name
	2,                                                // API major version number
	1,                                                // API minor version number
	0,                                                // Plugin major version number
	1,                                                // Plugin minor version number
	FF_SOURCE,                                        // Plugin type
	"Digital rain: falling columns of code",          // Plugin description
	"Downpour FFGL source"                            // About
);

extern "C" const char* DownpourSourceBuildStamp()
{
	return "downpour " DOWNPOUR_VERSION " source, built " __DATE__ " " __TIME__;
}
