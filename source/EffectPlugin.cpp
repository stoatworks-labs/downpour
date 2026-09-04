#include "Downpour.h"

/**
    The effect: the same rain, over the incoming clip.

    Exists because the obvious way to get rain over footage in Resolume -- put
    the source on its own layer and pick a blend mode -- costs a layer and puts
    the rain's controls somewhere other than the clip it belongs to. This one
    sits on the clip.

    The compositing order is deliberate and is shared with the source: the clip
    is at the bottom, the plugin's own background veils it, and the rain falls
    on top. So Background Alpha keeps exactly the meaning it has in the source
    plugin -- how much of the plugin's own backdrop you see -- rather than
    becoming a second, differently-behaved control on this side.

    See SourcePlugin.cpp for why this file is listed in its own target rather
    than in the shared library.
*/
namespace
{
class DownpourEffect : public downpour::DownpourPlugin
{
public:
	DownpourEffect() :
		DownpourPlugin( true )
	{
	}
};
} // namespace

static CFFGLPluginInfo PluginInfo(
	PluginFactory< DownpourEffect >,                  // Create method
	"DP02",                                           // Plugin unique ID of maximum length 4
	"Downpour Over",                                  // Plugin name
	2,                                                // API major version number
	1,                                                // API minor version number
	0,                                                // Plugin major version number
	1,                                                // Plugin minor version number
	FF_EFFECT,                                        // Plugin type
	"Digital rain over the clip: falling columns of characters.\n\nUse the built-in face or point it at any TTF or OTF on the machine. There is a built-in one on purpose - a composition saved on one rig carries a font path, and the next rig is in a different country. When the path does not resolve the picture still arrives.\n\nStart from a Preset, at the bottom.",// Plugin description
	"Downpour FFGL effect"                            // About
);

extern "C" const char* DownpourEffectBuildStamp()
{
	return "downpour " DOWNPOUR_VERSION " effect, built " __DATE__ " " __TIME__;
}
