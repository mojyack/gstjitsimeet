#include "jitsibin.hpp"

namespace {
auto register_callback(GstPlugin* const plugin) -> gboolean {
    return gst_element_register(plugin, "jitsibin", GST_RANK_NONE, gst_jitsibin_get_type());
}

const auto plugin_desc = GstPluginDesc{
    GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    "gstjitsimeet",           // plugin name
    "Jitsi Meet gst binding", // description
    register_callback,        // plugin init
    "1.0",                    // version
    "LGPL",                   // licence
    "unknown",                // source
    "unknown",                // package
    "@mojyack",               // origin
    NULL,                     // release datetime
    {NULL},
};
} // namespace

extern "C" {
auto gst_plugin_jitsimeet_get_desc() -> const GstPluginDesc* {
    return &plugin_desc;
}
}
