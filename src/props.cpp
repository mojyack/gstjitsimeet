#include "props.hpp"
#include "jitsi/assert.hpp"

auto Props::ensure_required_prop() const -> bool {
    assert_b(!server_address.empty(), "please set server");
    assert_b(!room_name.empty(), "please set room");
    return true;
}

auto Props::handle_set_prop(const guint id, const GValue* const value, GParamSpec* const /*spec*/) -> bool {
    switch(id) {
    case server_address_id:
        server_address = g_value_get_string(value);
        return true;
    case room_name_id:
        room_name = g_value_get_string(value);
        return true;
    case last_n_id:
        last_n = g_value_get_int(value);
        return true;
    case secure_id:
        secure = g_value_get_boolean(value) == FALSE;
        return true;
    case async_sink_id:
        async_sink = g_value_get_boolean(value) == TRUE;
        return true;
    default:
        return false;
    }
}

auto Props::handle_get_prop(const guint id, GValue* const value, GParamSpec* const /*spec*/) -> bool {
    switch(id) {
    case server_address_id:
        g_value_set_string(value, server_address.data());
        return true;
    case room_name_id:
        g_value_set_string(value, room_name.data());
        return true;
    case last_n_id:
        g_value_set_int(value, last_n);
        return true;
    case secure_id:
        g_value_set_boolean(value, !secure ? TRUE : FALSE);
        return true;
    case async_sink_id:
        g_value_set_boolean(value, async_sink ? TRUE : FALSE);
        return true;
    default:
        return false;
    }
}

auto Props::install_props(GObjectClass* const obj) -> void {
    g_object_class_install_property(obj, server_address_id,
                                    g_param_spec_string("server",
                                                        NULL,
                                                        "FQDN of jitsi meet server",
                                                        "",
                                                        GParamFlags(G_PARAM_READWRITE)));
    g_object_class_install_property(obj, room_name_id,
                                    g_param_spec_string("room",
                                                        NULL,
                                                        "Room name of the conference",
                                                        "",
                                                        GParamFlags(G_PARAM_READWRITE)));
    g_object_class_install_property(obj, last_n_id,
                                    g_param_spec_int("receive-limit",
                                                     NULL,
                                                     "Maximum number of participants to receive streams from (-1 for unlimit)",
                                                     -1, std::numeric_limits<int>::max(), 0,
                                                     GParamFlags(G_PARAM_READWRITE)));
    g_object_class_install_property(obj, secure_id,
                                    g_param_spec_boolean("insecure",
                                                         NULL,
                                                         "Trust server self-signed certification",
                                                         FALSE,
                                                         GParamFlags(G_PARAM_READWRITE)));
    g_object_class_install_property(obj, async_sink_id,
                                    g_param_spec_boolean("force-play",
                                                         NULL,
                                                         "Force pipeline to play even in conference with no participants",
                                                         FALSE,
                                                         GParamFlags(G_PARAM_READWRITE)));
}
