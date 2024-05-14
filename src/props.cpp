#include <gst/gstutils.h>

#include "jitsi/assert.hpp"
#include "jitsi/config.hpp"
#include "props.hpp"

namespace {
enum class AudioCodecType {
    Opus = 1,
};

enum class VideoCodecType {
    H264 = 1,
    Vp8,
    Vp9,
};

auto audio_codec_type_get_type() -> GType {
    static auto type = GType(0);
    if(type != 0) {
        return type;
    }

    static const auto value = std::array{
        GEnumValue{static_cast<guint>(AudioCodecType::Opus), "opus", "Opus"},
        GEnumValue{0, NULL, NULL},
    };

    type = g_enum_register_static("AudioCodecType", value.data());

    return type;
}

auto video_codec_type_get_type() -> GType {
    static auto type = GType(0);
    if(type != 0) {
        return type;
    }

    static const auto value = std::array{
        GEnumValue{static_cast<guint>(VideoCodecType::H264), "h264", "H.264"},
        GEnumValue{static_cast<guint>(VideoCodecType::Vp8), "vp8", "VP8"},
        GEnumValue{static_cast<guint>(VideoCodecType::Vp9), "vp9", "VP9"},
        GEnumValue{0, NULL, NULL},
    };

    type = g_enum_register_static("VideoCodecType", value.data());

    return type;
}
} // namespace

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
    case audio_codec_type_id:
        switch(AudioCodecType(g_value_get_enum(value))) {
        case AudioCodecType::Opus:
            audio_codec_type = CodecType::Opus;
            return true;
        default:
            return false;
        }
    case video_codec_type_id:
        switch(VideoCodecType(g_value_get_enum(value))) {
        case VideoCodecType::H264:
            video_codec_type = CodecType::H264;
            return true;
        case VideoCodecType::Vp8:
            video_codec_type = CodecType::Vp8;
            return true;
        case VideoCodecType::Vp9:
            video_codec_type = CodecType::Vp9;
            return true;
        default:
            return false;
        }
    case last_n_id:
        last_n = g_value_get_int(value);
        return true;
    case jitterbuffer_latency_id:
        jitterbuffer_latency = g_value_get_uint(value);
        return true;
    case secure_id:
        secure = g_value_get_boolean(value) == FALSE;
        return true;
    case async_sink_id:
        async_sink = g_value_get_boolean(value) == TRUE;
        return true;
    case libws_loglevel_bitmap_id:
        config::libws_loglevel_bitmap = g_value_get_uint(value);
        return true;
    case dump_websocket_packets_id:
        config::dump_websocket_packets = g_value_get_boolean(value) == TRUE;
        return true;
    case debug_websocket_id:
        config::debug_websocket = g_value_get_boolean(value) == TRUE;
        return true;
    case debug_xmpp_connection_id:
        config::debug_xmpp_connection = g_value_get_boolean(value) == TRUE;
        return true;
    case debug_conference_id:
        config::debug_conference = g_value_get_boolean(value) == TRUE;
        return true;
    case debug_jingle_handler_id:
        config::debug_jingle_handler = g_value_get_boolean(value) == TRUE;
        return true;
    case debug_ice_id:
        config::debug_ice = g_value_get_boolean(value) == TRUE;
        return true;
    case debug_colibri_id:
        config::debug_colibri = g_value_get_boolean(value) == TRUE;
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
    case audio_codec_type_id:
        switch(audio_codec_type) {
        case CodecType::Opus:
            g_value_set_enum(value, gint(AudioCodecType::Opus));
            return true;
        default:
            return false;
        }
    case video_codec_type_id:
        switch(video_codec_type) {
        case CodecType::H264:
            g_value_set_enum(value, gint(CodecType::H264));
            return true;
        case CodecType::Vp8:
            g_value_set_enum(value, gint(CodecType::Vp8));
            return true;
        case CodecType::Vp9:
            g_value_set_enum(value, gint(CodecType::Vp9));
            return true;
        default:
            return false;
        }
    case last_n_id:
        g_value_set_int(value, last_n);
        return true;
    case jitterbuffer_latency_id:
        g_value_set_uint(value, jitterbuffer_latency);
        return true;
    case secure_id:
        g_value_set_boolean(value, !secure ? TRUE : FALSE);
        return true;
    case async_sink_id:
        g_value_set_boolean(value, async_sink ? TRUE : FALSE);
        return true;
    case libws_loglevel_bitmap_id:
        g_value_set_uint(value, config::libws_loglevel_bitmap);
        return true;
    case dump_websocket_packets_id:
        g_value_set_boolean(value, config::dump_websocket_packets ? TRUE : FALSE);
        return true;
    case debug_websocket_id:
        g_value_set_boolean(value, config::debug_websocket ? TRUE : FALSE);
        return true;
    case debug_xmpp_connection_id:
        g_value_set_boolean(value, config::debug_xmpp_connection ? TRUE : FALSE);
        return true;
    case debug_conference_id:
        g_value_set_boolean(value, config::debug_conference ? TRUE : FALSE);
        return true;
    case debug_jingle_handler_id:
        g_value_set_boolean(value, config::debug_jingle_handler ? TRUE : FALSE);
        return true;
    case debug_ice_id:
        g_value_set_boolean(value, config::debug_ice ? TRUE : FALSE);
        return true;
    case debug_colibri_id:
        g_value_set_boolean(value, config::debug_colibri ? TRUE : FALSE);
        return true;
    default:
        return false;
    }
}

auto Props::install_props(GObjectClass* const obj) -> void {
    const auto bool_prop = [obj](const int         id,
                                 const char* const name,
                                 const char* const desc,
                                 const gboolean    def,
                                 const bool        init) -> void {
        const auto flag = init ? GParamFlags(G_PARAM_READWRITE | G_PARAM_CONSTRUCT) : GParamFlags(G_PARAM_READWRITE);
        g_object_class_install_property(obj, id,
                                        g_param_spec_boolean(name,
                                                             NULL,
                                                             desc,
                                                             def,
                                                             flag));
    };

    g_object_class_install_property(obj, server_address_id,
                                    g_param_spec_string("server",
                                                        NULL,
                                                        "FQDN of jitsi meet server",
                                                        NULL,
                                                        GParamFlags(G_PARAM_READWRITE)));
    g_object_class_install_property(obj, room_name_id,
                                    g_param_spec_string("room",
                                                        NULL,
                                                        "Room name of the conference",
                                                        NULL,
                                                        GParamFlags(G_PARAM_READWRITE)));
    g_object_class_install_property(obj, audio_codec_type_id,
                                    g_param_spec_enum("audio-codec",
                                                      NULL,
                                                      "Audio codec to send",
                                                      audio_codec_type_get_type(),
                                                      guint(AudioCodecType::Opus),
                                                      GParamFlags(G_PARAM_READWRITE | G_PARAM_CONSTRUCT)));
    g_object_class_install_property(obj, video_codec_type_id,
                                    g_param_spec_enum("video-codec",
                                                      NULL,
                                                      "Video codec to send",
                                                      video_codec_type_get_type(),
                                                      guint(VideoCodecType::H264),
                                                      GParamFlags(G_PARAM_READWRITE | G_PARAM_CONSTRUCT)));
    g_object_class_install_property(obj, jitterbuffer_latency_id,
                                    g_param_spec_uint("jitterbuffer-latency",
                                                      NULL,
                                                      "Jitterbuffer latency in milliseconds",
                                                      0, std::numeric_limits<uint>::max(), 200,
                                                      GParamFlags(G_PARAM_READWRITE | G_PARAM_CONSTRUCT)));
    g_object_class_install_property(obj, last_n_id,
                                    g_param_spec_int("receive-limit",
                                                     NULL,
                                                     "Maximum number of participants to receive streams from (-1 for unlimit)",
                                                     -1, std::numeric_limits<int>::max(), 0,
                                                     GParamFlags(G_PARAM_READWRITE | G_PARAM_CONSTRUCT)));
    bool_prop(secure_id, "insecure", "Trust server self-signed certification", FALSE, true);
    bool_prop(async_sink_id, "force-play", "Force pipeline to play even in conference with no participants", FALSE, true);
    bool_prop(verbose_id, "verbose", "Enable debug messages", FALSE, true);
    g_object_class_install_property(obj, libws_loglevel_bitmap_id,
                                    g_param_spec_uint("lws-loglevel-bitmap",
                                                      NULL,
                                                      "libwebsockets lws_set_log_level value",
                                                      0, std::numeric_limits<int>::max(), 0b11, // LLL_ERR | LLL_WARN
                                                      GParamFlags(G_PARAM_READWRITE | G_PARAM_CONSTRUCT)));
    bool_prop(dump_websocket_packets_id, "dump-websocket-packets", "Print websocket packets", FALSE, true);
    bool_prop(debug_websocket_id, "debug-websocket", "Enable websocket debug messages", FALSE, true);
    bool_prop(debug_xmpp_connection_id, "debug-xmpp-negotiation", "Enable xmpp negotiator debug messages", FALSE, true);
    bool_prop(debug_conference_id, "debug-conference", "Enable conference debug messages", FALSE, true);
    bool_prop(debug_jingle_handler_id, "debug-jingle", "Enable jingle debug messages", FALSE, true);
    bool_prop(debug_ice_id, "debug-ice", "Enable ice debug messages", FALSE, true);
    bool_prop(debug_colibri_id, "debug-colibri", "Enable colibri debug messages", FALSE, true);

    gst_type_mark_as_plugin_api(audio_codec_type_get_type(), GstPluginAPIFlags(0));
    gst_type_mark_as_plugin_api(video_codec_type_get_type(), GstPluginAPIFlags(0));
}
