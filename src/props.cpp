#include <array>
#include <limits>

#include <gst/gstutils.h>

#include "macros/unwrap.hpp"
#include "props.hpp"
#include "util/pair-table.hpp"

namespace {
enum class AudioCodecType {
    Opus = 1,
};

enum class VideoCodecType {
    H264 = 1,
    Vp8,
    Vp9,
    Av1,
};

auto audio_codec_type_table = make_pair_table<CodecType, AudioCodecType>({
    {CodecType::Opus, AudioCodecType::Opus},
});

auto video_codec_type_table = make_pair_table<CodecType, VideoCodecType>({
    {CodecType::H264, VideoCodecType::H264},
    {CodecType::Vp8, VideoCodecType::Vp8},
    {CodecType::Vp9, VideoCodecType::Vp9},
    {CodecType::Av1, VideoCodecType::Av1},
});

auto audio_codec_type_get_type() -> GType {
    static auto type = GType(0);
    if(type != 0) {
        return type;
    }

    static const auto value = std::array{
        GEnumValue{std::to_underlying(AudioCodecType::Opus), "opus", "Opus"},
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
        GEnumValue{std::to_underlying(VideoCodecType::H264), "h264", "H.264"},
        GEnumValue{std::to_underlying(VideoCodecType::Vp8), "vp8", "VP8"},
        GEnumValue{std::to_underlying(VideoCodecType::Vp9), "vp9", "VP9"},
        GEnumValue{std::to_underlying(VideoCodecType::Av1), "av1", "AV1"},
        GEnumValue{0, NULL, NULL},
    };

    type = g_enum_register_static("VideoCodecType", value.data());

    return type;
}
} // namespace

auto Props::ensure_required_prop() const -> bool {
    ensure(!server_address.empty(), "please set server");
    ensure(!room_name.empty(), "please set room");
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
    case nick_id:
        nick = g_value_get_string(value);
        return true;
    case audio_codec_type_id: {
        unwrap(type, audio_codec_type_table.find(AudioCodecType(g_value_get_enum(value))));
        audio_codec_type = type;
        return true;
    }
    case video_codec_type_id: {
        unwrap(type, video_codec_type_table.find(VideoCodecType(g_value_get_enum(value))));
        video_codec_type = type;
        return true;
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
    case nick_id:
        g_value_set_string(value, nick.data());
        return true;
    case audio_codec_type_id: {
        unwrap(type, audio_codec_type_table.find(audio_codec_type));
        g_value_set_enum(value, std::to_underlying(type));
        return true;
    }
    case video_codec_type_id: {
        unwrap(type, video_codec_type_table.find(video_codec_type));
        g_value_set_enum(value, std::to_underlying(type));
        return true;
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
    default:
        return false;
    }
}

auto Props::install_props(GObjectClass* const obj) -> void {
    constexpr auto rw           = GParamFlags(G_PARAM_READWRITE);
    constexpr auto rw_construct = GParamFlags(G_PARAM_READWRITE | G_PARAM_CONSTRUCT);

    const auto bool_prop = [obj](const int         id,
                                 const char* const name,
                                 const char* const desc,
                                 const gboolean    def) -> void {
        g_object_class_install_property(obj, id, g_param_spec_boolean(name, NULL, desc, def, rw_construct));
    };

    g_object_class_install_property(
        obj, server_address_id,
        g_param_spec_string("server",
                            NULL,
                            "FQDN of jitsi meet server",
                            NULL,
                            rw));

    g_object_class_install_property(
        obj, room_name_id,
        g_param_spec_string("room",
                            NULL,
                            "Room name of the conference",
                            NULL,
                            rw));

    g_object_class_install_property(
        obj, nick_id,
        g_param_spec_string("nick",
                            NULL,
                            "Nick name of this participant",
                            "gstjitsimeet",
                            rw_construct));

    g_object_class_install_property(
        obj, audio_codec_type_id,
        g_param_spec_enum("audio-codec",
                          NULL,
                          "Audio codec to send",
                          audio_codec_type_get_type(),
                          guint(AudioCodecType::Opus),
                          rw_construct));

    g_object_class_install_property(
        obj, video_codec_type_id,
        g_param_spec_enum("video-codec",
                          NULL,
                          "Video codec to send",
                          video_codec_type_get_type(),
                          guint(VideoCodecType::H264),
                          rw_construct));

    g_object_class_install_property(
        obj, jitterbuffer_latency_id,
        g_param_spec_uint("jitterbuffer-latency",
                          NULL,
                          "Jitterbuffer latency in milliseconds",
                          0, std::numeric_limits<guint>::max(), 200,
                          rw_construct));

    g_object_class_install_property(
        obj, last_n_id,
        g_param_spec_int("receive-limit",
                         NULL,
                         "Maximum number of participants to receive streams from (-1 for unlimit)",
                         -1, std::numeric_limits<int>::max(), 0,
                         rw_construct));

    bool_prop(secure_id, "insecure", "Trust server self-signed certification", FALSE);
    bool_prop(async_sink_id, "force-play", "Force pipeline to play even in conference with no participants", FALSE);

    gst_type_mark_as_plugin_api(audio_codec_type_get_type(), GstPluginAPIFlags(0));
    gst_type_mark_as_plugin_api(video_codec_type_get_type(), GstPluginAPIFlags(0));
}
