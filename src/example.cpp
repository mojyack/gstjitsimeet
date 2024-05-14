#include <gst/gst.h>
#include <gst/video/gstvideodecoder.h>

#include "auto-gst-object.hpp"
#include "jitsi/autoptr.hpp"
#include "jitsi/unwrap.hpp"
#include "util/charconv.hpp"
#include "util/misc.hpp"

namespace {
declare_autoptr(GMainLoop, GMainLoop, g_main_loop_unref);
declare_autoptr(GstMessage, GstMessage, gst_message_unref);
declare_autoptr(GString, gchar, g_free);

// helpers
auto add_new_element_to_pipeine(GstElement* const pipeline, const char* const element_name) -> GstElement* {
    auto elm = gst_element_factory_make(element_name, NULL);
    assert_p(elm != NULL);
    assert_p(gst_bin_add(GST_BIN(pipeline), elm) == TRUE);
    assert_p(gst_element_sync_state_with_parent(elm));
    return elm;
}

// callbacks
struct Context {
    GstElement* pipeline;
    GstElement* videotestsrc;
};

auto jitsibin_pad_added_handler(GstElement* const jitsibin, GstPad* const pad, gpointer const data) -> void {
    auto& self = *std::bit_cast<Context*>(data);

    const auto name_g = AutoGString(gst_object_get_name(GST_OBJECT(pad)));
    const auto name   = std::string_view(name_g.get());
    PRINT("pad added name=", name);

    const auto elms = split(name, "_");
    assert_n(elms.size() == 3, "malformed pad name");
    const auto participant_id = elms[0];
    const auto codec          = elms[1];
    unwrap_on(ssrc, from_chars<uint32_t>(elms[2]));
    (void)participant_id;
    (void)ssrc;

    auto decoder = std::string();
    // TODO: handle all codec type
    if(codec == "OPUS") {
        decoder = "TODO";
    } else if(codec == "H264") {
        decoder = "avdec_h264";
    } else if(codec == "VP8") {
        decoder = "TODO";
    } else if(codec == "VP9") {
        decoder = "TODO";
    } else {
        PRINT("unsupported codec: ", codec);
        decoder = "fakesink";
        return;
    }

    if(decoder == "TODO") {
        unwrap_pn_mut(fakesink, add_new_element_to_pipeine(self.pipeline, "fakesink"));
        const auto fakesink_sink_pad = AutoGstObject(gst_element_get_static_pad(&fakesink, "sink"));
        assert_n(fakesink_sink_pad.get() != NULL);
        assert_n(gst_pad_link(pad, fakesink_sink_pad.get()) == GST_PAD_LINK_OK);
        return;
    }

    // for video
    unwrap_pn_mut(dec, add_new_element_to_pipeine(self.pipeline, decoder.data()));
    unwrap_pn_mut(videoconvert, add_new_element_to_pipeine(self.pipeline, "videoconvert"));
    unwrap_pn_mut(waylandsink, add_new_element_to_pipeine(self.pipeline, "waylandsink"));
    g_object_set(&dec,
                 "automatic-request-sync-points", TRUE,
                 "automatic-request-sync-point-flags", GST_VIDEO_DECODER_REQUEST_SYNC_POINT_CORRUPT_OUTPUT,
                 NULL);

    const auto dec_sink_pad = AutoGstObject(gst_element_get_static_pad(&dec, "sink"));
    assert_n(dec_sink_pad.get() != NULL);
    assert_n(gst_pad_link(pad, GST_PAD(dec_sink_pad.get())) == GST_PAD_LINK_OK);
    assert_n(gst_element_link_pads(&dec, NULL, &videoconvert, NULL) == TRUE);
    assert_n(gst_element_link_pads(&videoconvert, NULL, &waylandsink, NULL) == TRUE);
    PRINT("added h264 decoder");
}

auto jitsibin_pad_removed_handler(GstElement* const jitisbin, GstPad* const pad, gpointer const data) -> void {
    const auto name_g = AutoGString(gst_object_get_name(GST_OBJECT(pad)));
    const auto name   = std::string_view(name_g.get());
    PRINT("pad removed name=", name);
}

auto jitsibin_participant_joined_handler(GstElement* const jitisbin, const gchar* const participant_id, const gchar* const nick) -> void {
    PRINT("participant joined ", participant_id, " ", nick);
}

auto jitsibin_participant_left_handler(GstElement* const jitisbin, const gchar* const participant_id, const gchar* const nick) -> void {
    PRINT("participant left ", participant_id, " ", nick);
}

auto run_pipeline(GstElement* pipeline) -> bool {
    assert_b(gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_SUCCESS);

    const auto bus = AutoGstObject(gst_element_get_bus(pipeline));
    assert_b(bus.get() != NULL);
    const auto msg = AutoGstMessage(gst_bus_timed_pop_filtered(bus.get(),
                                                               GST_CLOCK_TIME_NONE,
                                                               GstMessageType(GST_MESSAGE_ERROR | GST_MESSAGE_EOS)));
    assert_b(msg.get() != NULL);
    switch(GST_MESSAGE_TYPE(msg.get())) {
    case GST_MESSAGE_ERROR: {
        auto err = (GError*)(NULL);
        auto str = (gchar*)(NULL);
        gst_message_parse_error(msg.get(), &err, &str);
        g_printerr("Error received from element %s: %s\n",
                   GST_OBJECT_NAME(msg->src), err->message);
        g_printerr("Debugging information: %s\n",
                   str ? str : "none");
        g_clear_error(&err);
        g_free(str);
    }
    case GST_MESSAGE_EOS:
        g_print("End-Of-Stream reached.\n");
        break;
    default:
        g_printerr("Unexpected message received.\n");
        break;
    }

    return true;
}

auto run() -> bool {
    const auto pipeline = AutoGstObject(gst_pipeline_new(NULL));
    assert_b(pipeline.get() != NULL);

    auto context = Context{
        .pipeline = pipeline.get(),
    };

    /*
     * videotestsrc -> tee -> waylandsink
     *                     -> videoconvert -> x264enc -> jitisbin
     * audiotestsrc ->                        opusenc ->
     */

    unwrap_pb_mut(videotestsrc, add_new_element_to_pipeine(pipeline.get(), "videotestsrc"));
    unwrap_pb_mut(tee, add_new_element_to_pipeine(pipeline.get(), "tee"));
    unwrap_pb_mut(waylandsink, add_new_element_to_pipeine(pipeline.get(), "waylandsink"));
    unwrap_pb_mut(videoconvert, add_new_element_to_pipeine(pipeline.get(), "videoconvert"));
    unwrap_pb_mut(x264enc, add_new_element_to_pipeine(pipeline.get(), "x264enc"));
    unwrap_pb_mut(audiotestsrc, add_new_element_to_pipeine(pipeline.get(), "audiotestsrc"));
    unwrap_pb_mut(opusenc, add_new_element_to_pipeine(pipeline.get(), "opusenc"));
    unwrap_pb_mut(jitsibin, add_new_element_to_pipeine(pipeline.get(), "jitsibin"));
    g_signal_connect(&jitsibin, "pad-added", G_CALLBACK(jitsibin_pad_added_handler), &context);
    g_signal_connect(&jitsibin, "pad-removed", G_CALLBACK(jitsibin_pad_removed_handler), &context);
    g_signal_connect(&jitsibin, "participant-joined", G_CALLBACK(jitsibin_participant_joined_handler), &context);
    g_signal_connect(&jitsibin, "participant-left", G_CALLBACK(jitsibin_participant_left_handler), &context);

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
    g_object_set(&jitsibin,
                 "server", "jitsi.local",
                 "room", "room",
                 "receive-limit", 3,
                 "force-play", TRUE,
                 "insecure", TRUE,
                 NULL);

    assert_b(gst_element_link_pads(&videotestsrc, NULL, &tee, NULL) == TRUE);
    assert_b(gst_element_link_pads(&tee, NULL, &waylandsink, NULL) == TRUE);
    assert_b(gst_element_link_pads(&tee, NULL, &videoconvert, NULL) == TRUE);
    assert_b(gst_element_link_pads(&videoconvert, NULL, &x264enc, NULL) == TRUE);
    assert_b(gst_element_link_pads(&x264enc, NULL, &jitsibin, "video_sink") == TRUE);
    assert_b(gst_element_link_pads(&audiotestsrc, NULL, &opusenc, NULL) == TRUE);
    assert_b(gst_element_link_pads(&opusenc, NULL, &jitsibin, "audio_sink") == TRUE);

    return run_pipeline(pipeline.get());
}
} // namespace

auto main(int argc, char* argv[]) -> int {
    gst_init(&argc, &argv);
    return run() ? 1 : 0;
}
