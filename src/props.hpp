#pragma once
#include <string>

#include <glib-object.h>

#include "jitsi/codec-type.hpp"

struct Props {
    enum {
        server_address_id = 1,
        room_name_id,
        nick_id,
        audio_codec_type_id,
        video_codec_type_id,
        last_n_id,
        jitterbuffer_latency_id,
        secure_id,
        async_sink_id,
        verbose_id,
        // libjitsimeet config
        libws_loglevel_bitmap_id,
        dump_websocket_packets_id,
        debug_websocket_id,
        debug_xmpp_connection_id,
        debug_conference_id,
        debug_jingle_handler_id,
        debug_ice_id,
        debug_colibri_id,
    };

    std::string server_address;
    std::string room_name;
    std::string nick;
    CodecType   audio_codec_type;
    CodecType   video_codec_type;
    int         last_n;
    guint       jitterbuffer_latency;
    bool        secure;
    bool        async_sink;
    bool        verbose;

    auto ensure_required_prop() const -> bool;
    auto handle_set_prop(const guint id, const GValue* value, GParamSpec* spec) -> bool;
    auto handle_get_prop(const guint id, GValue* value, GParamSpec* spec) -> bool;

    static auto install_props(GObjectClass* obj) -> void;
};
