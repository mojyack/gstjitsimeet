#pragma once
#include <string>

#include <glib-object.h>

struct Props {
    enum {
        server_address_id = 1,
        room_name_id,
        last_n_id,
        secure_id,
        async_sink_id,
    };

    std::string server_address;
    std::string room_name;
    int         last_n     = 0;
    bool        secure     = true;
    bool        async_sink = false;

    auto ensure_required_prop() const -> bool;
    auto handle_set_prop(const guint id, const GValue* value, GParamSpec* spec) -> bool;
    auto handle_get_prop(const guint id, GValue* value, GParamSpec* spec) -> bool;

    static auto install_props(GObjectClass* obj) -> void;
};