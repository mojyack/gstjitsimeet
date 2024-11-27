#include <gst/gst.h>
#include <gst/video/gstvideodecoder.h>

#include "../gstutil/auto-gst-object.hpp"
#include "../gstutil/pipeline-helper.hpp"
#include "../macros/autoptr.hpp"
#include "../macros/unwrap.hpp"
#include "../util/argument-parser.hpp"
#include "helper.hpp"

namespace {
declare_autoptr(GMainLoop, GMainLoop, g_main_loop_unref);
declare_autoptr(GstMessage, GstMessage, gst_message_unref);
declare_autoptr(GString, gchar, g_free);

// callbacks
struct Context {
    GstElement* pipeline;
    GstElement* videotestsrc;
};

auto jitsibin_pad_added_handler(GstElement* const /*jitsibin*/, GstPad* const pad, gpointer const data) -> void {
    auto& self = *std::bit_cast<Context*>(data);

    const auto name_g = AutoGString(gst_object_get_name(GST_OBJECT(pad)));
    const auto name   = std::string_view(name_g.get());
    line_print("pad added name=", name);

    unwrap(pad_name, parse_jitsibin_pad_name(name));

    auto decoder = std::string();
    // TODO: handle all codec type
    if(pad_name.codec == "OPUS") {
        decoder = "TODO";
    } else if(pad_name.codec == "H264") {
        decoder = "avdec_h264";
    } else if(pad_name.codec == "VP8") {
        decoder = "avdec_vp8";
    } else if(pad_name.codec == "VP9") {
        decoder = "TODO";
    } else {
        line_print("unsupported codec: ", pad_name.codec);
        decoder = "fakesink";
        return;
    }

    if(decoder == "TODO") {
        unwrap_mut(fakesink, add_new_element_to_pipeine(self.pipeline, "fakesink"));
        const auto fakesink_sink_pad = AutoGstObject(gst_element_get_static_pad(&fakesink, "sink"));
        ensure(fakesink_sink_pad.get() != NULL);
        ensure(gst_pad_link(pad, fakesink_sink_pad.get()) == GST_PAD_LINK_OK);
        ensure(gst_element_sync_state_with_parent(&fakesink) == TRUE);
        return;
    }

    // for video
    unwrap_mut(dec, add_new_element_to_pipeine(self.pipeline, decoder.data()));
    unwrap_mut(videoconvert, add_new_element_to_pipeine(self.pipeline, "videoconvert"));
    unwrap_mut(waylandsink, add_new_element_to_pipeine(self.pipeline, "waylandsink"));
    g_object_set(&dec,
                 "automatic-request-sync-points", TRUE,
                 "automatic-request-sync-point-flags", GST_VIDEO_DECODER_REQUEST_SYNC_POINT_CORRUPT_OUTPUT,
                 NULL);

    const auto dec_sink_pad = AutoGstObject(gst_element_get_static_pad(&dec, "sink"));
    ensure(dec_sink_pad.get() != NULL);
    ensure(gst_pad_link(pad, GST_PAD(dec_sink_pad.get())) == GST_PAD_LINK_OK);
    ensure(gst_element_link_pads(&dec, NULL, &videoconvert, NULL) == TRUE);
    ensure(gst_element_link_pads(&videoconvert, NULL, &waylandsink, NULL) == TRUE);
    ensure(gst_element_sync_state_with_parent(&videoconvert) == TRUE);
    ensure(gst_element_sync_state_with_parent(&waylandsink) == TRUE);
    ensure(gst_element_sync_state_with_parent(&dec) == TRUE);
    line_print("added h264 decoder");
}

auto jitsibin_pad_removed_handler(GstElement* const /*jitisbin*/, GstPad* const pad, gpointer const /*data*/) -> void {
    const auto name_g = AutoGString(gst_object_get_name(GST_OBJECT(pad)));
    const auto name   = std::string_view(name_g.get());
    line_print("pad removed name=", name);
}

auto jitsibin_participant_joined_handler(GstElement* const /*jitisbin*/, const gchar* const participant_id, const gchar* const nick, gpointer const /*data*/) -> void {
    line_print("participant joined ", participant_id, " ", nick);
}

auto jitsibin_participant_left_handler(GstElement* const /*jitisbin*/, const gchar* const participant_id, const gchar* const nick, gpointer const /*data*/) -> void {
    line_print("participant left ", participant_id, " ", nick);
}

auto jitsibin_mute_state_changed_handler(GstElement* const /*jitisbin*/, const gchar* const participant_id, const gboolean is_audio, const gboolean new_muted, gpointer const /*data*/) -> void {
    line_print("mute state changed ", participant_id, " ", is_audio ? "audio" : "video", "=", new_muted);
}
} // namespace

auto main(const int argc, const char* const* argv) -> int {
    const char* host = nullptr;
    const char* room = nullptr;
    {
        auto help   = false;
        auto parser = args::Parser<>();
        parser.arg(&host, "HOST", "server domain");
        parser.arg(&room, "ROOM", "room name");
        parser.kwflag(&help, {"-h", "--help"}, "print this help message", {.no_error_check = true});
        if(!parser.parse(argc, argv) || help) {
            print("usage: example ", parser.get_help());
            return 0;
        }
    }

    gst_init(NULL, NULL);

    const auto pipeline = AutoGstObject(gst_pipeline_new(NULL));
    ensure(pipeline.get() != NULL);

    auto context = Context{
        .pipeline = pipeline.get(),
    };

    /*
     * videotestsrc -> tee -> waylandsink
     *                     -> videoconvert -> x264enc -> jitisbin
     * audiotestsrc ->                        opusenc ->
     */

    unwrap_mut(videotestsrc, add_new_element_to_pipeine(pipeline.get(), "videotestsrc"));
    unwrap_mut(tee, add_new_element_to_pipeine(pipeline.get(), "tee"));
    unwrap_mut(waylandsink, add_new_element_to_pipeine(pipeline.get(), "waylandsink"));
    unwrap_mut(videoconvert, add_new_element_to_pipeine(pipeline.get(), "videoconvert"));
    unwrap_mut(x264enc, add_new_element_to_pipeine(pipeline.get(), "x264enc"));
    unwrap_mut(audiotestsrc, add_new_element_to_pipeine(pipeline.get(), "audiotestsrc"));
    unwrap_mut(opusenc, add_new_element_to_pipeine(pipeline.get(), "opusenc"));
    unwrap_mut(jitsibin, add_new_element_to_pipeine(pipeline.get(), "jitsibin"));
    g_signal_connect(&jitsibin, "pad-added", G_CALLBACK(jitsibin_pad_added_handler), &context);
    g_signal_connect(&jitsibin, "pad-removed", G_CALLBACK(jitsibin_pad_removed_handler), &context);
    g_signal_connect(&jitsibin, "participant-joined", G_CALLBACK(jitsibin_participant_joined_handler), &context);
    g_signal_connect(&jitsibin, "participant-left", G_CALLBACK(jitsibin_participant_left_handler), &context);
    g_signal_connect(&jitsibin, "mute-state-changed", G_CALLBACK(jitsibin_mute_state_changed_handler), &context);

    g_object_set(&waylandsink,
                 "async", FALSE,
                 NULL);
    g_object_set(&videotestsrc,
                 "is-live", TRUE,
                 NULL);
    g_object_set(&audiotestsrc,
                 "is-live", TRUE,
                 "wave", 8,
                 NULL);
    g_object_set(&x264enc,
                 "key-int-max", 30,
                 "tune", 0x04,
                 NULL);
    g_object_set(&jitsibin,
                 "server", host,
                 "room", room,
                 "nick", "gstjitsimeet-example",
                 "receive-limit", 3,
                 "force-play", TRUE,
                 "insecure", TRUE,
                 NULL);

    ensure(gst_element_link_pads(&videotestsrc, NULL, &tee, NULL) == TRUE);
    ensure(gst_element_link_pads(&tee, NULL, &waylandsink, NULL) == TRUE);
    ensure(gst_element_link_pads(&tee, NULL, &videoconvert, NULL) == TRUE);
    ensure(gst_element_link_pads(&videoconvert, NULL, &x264enc, NULL) == TRUE);
    ensure(gst_element_link_pads(&x264enc, NULL, &jitsibin, "video_sink") == TRUE);
    ensure(gst_element_link_pads(&audiotestsrc, NULL, &opusenc, NULL) == TRUE);
    ensure(gst_element_link_pads(&opusenc, NULL, &jitsibin, "audio_sink") == TRUE);

    ensure(run_pipeline(pipeline.get()));

    return 0;
}
