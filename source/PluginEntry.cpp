#include "Crumple.h"

/**
    The one registration.

    Listed directly in the Crumple MODULE target, not in crumple_core:
    `CFFGLPluginInfo` registers itself from a file-scope constructor nothing
    references by name, so in a STATIC archive the linker may drop it -- a
    bundle that loads, exports `plugMain`, and contains no plugins.

    `SW Crumple` is ten characters of the sixteen the unterminated FFGL name
    field allows. `oxbow probe` reads it back the way a host does.
*/
namespace
{
class CrumpleEffect : public crumple::CrumplePlugin
{
};
} // namespace

static CFFGLPluginInfo PluginInfo(
	PluginFactory< CrumpleEffect >,                       // Create method
	"CR01",                                               // Plugin unique ID of maximum length 4
	"SW Crumple",                                         // Plugin name
	2,                                                    // API major version number
	1,                                                    // API minor version number
	0,                                                    // Plugin major version number
	1,                                                    // Plugin minor version number
	FF_EFFECT,                                            // Plugin type
	"The picture printed on paper that has been crumpled and smoothed out, under a lamp. Paper bends but "
	"does not stretch, so where the sheet tilts the print is pulled in: it kinks at every crease, the facets "
	"take the lamp one by one and the ridges throw shadows.",
	"Crumple FFGL effect"                                 // About
);

extern "C" const char* CrumpleBuildStamp()
{
	return "crumple " CRUMPLE_VERSION ", built " __DATE__ " " __TIME__;
}
