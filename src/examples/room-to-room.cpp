#include <gst/gst.h>
#include <gst/video/gstvideodecoder.h>

#include "../gstutil/auto-gst-object.hpp"
#include "../gstutil/pipeline-helper.hpp"
#include "../macros/autoptr.hpp"
#include "../macros/unwrap.hpp"
#include "../util/charconv.hpp"
#include "../util/misc.hpp"

namespace {
declare_autoptr(GMainLoop, GMainLoop, g_main_loop_unref);
declare_autoptr(GstMessage, GstMessage, gst_message_unref);
declare_autoptr(GString, gchar, g_free);
declare_autoptr(GstCaps, GstCaps, gst_caps_unref);

// callbacks
struct Context {
    GstElement* pipeline;
    GstElement* jitsibin_src;
    GstElement* jitsibin_sink;

    bool audio_connected = false;
    bool video_connected = false;
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

    auto audio_decoder_name = (const char*)(nullptr);
    auto video_decoder_name = (const char*)(nullptr);
    if(codec == "OPUS") {
        audio_decoder_name = "opusdec";
    } else if(codec == "H264") {
        video_decoder_name = "avdec_h264";
    } else if(codec == "VP8") {
        video_decoder_name = "avdec_vp8";
    } else if(codec == "VP9") {
        video_decoder_name = "avdec_vp9";
    } else {
        PRINT("unsupported codec: ", codec);
        return;
    }

    auto& connected = audio_decoder_name != nullptr ? self.audio_connected : self.video_connected;
    if(connected) {
        return;
    }

    if(video_decoder_name != nullptr) {
        const auto caps = AutoGstCaps(gst_caps_new_simple("video/x-raw",
                                                          "width", G_TYPE_INT, 320,
                                                          "height", G_TYPE_INT, 180,
                                                          NULL));

        // I tried to passthrough payloads without transcoding, but failed.
        // I had to resize video to smaller size to get it work.
        // Maybe bandwidth problem.
        // TODO: find colibri's receiverVideoConstraints can be used to limit video size,
        //       but I suspect this method is not reliable.

        //
        // (pad) -> avdec_* -> videoscale -> capsfilter -(320x180)> tee -> videoconvert -> waylandsink
        //                                                              -> videoconvert -> x264enc -> jitsibin
        //

        unwrap_pn_mut(dec, add_new_element_to_pipeine(self.pipeline, video_decoder_name));
        unwrap_pn_mut(videoscale, add_new_element_to_pipeine(self.pipeline, "videoscale"));
        unwrap_pn_mut(capsfilter, add_new_element_to_pipeine(self.pipeline, "capsfilter"));
        g_object_set(&capsfilter, "caps", caps.get(), NULL);
        unwrap_pn_mut(tee, add_new_element_to_pipeine(self.pipeline, "tee"));
        unwrap_pn_mut(videoconvert_wl, add_new_element_to_pipeine(self.pipeline, "videoconvert"));
        unwrap_pn_mut(waylandsink, add_new_element_to_pipeine(self.pipeline, "waylandsink"));
        unwrap_pn_mut(videoconvert_enc, add_new_element_to_pipeine(self.pipeline, "videoconvert"));
        unwrap_pn_mut(enc, add_new_element_to_pipeine(self.pipeline, "x264enc"));
        const auto dec_sink_pad = AutoGstObject(gst_element_get_static_pad(&dec, "sink"));
        assert_n(gst_pad_link(pad, dec_sink_pad.get()) == GST_PAD_LINK_OK);
        assert_n(gst_element_link_pads(&dec, NULL, &videoscale, NULL) == TRUE);
        assert_n(gst_element_link_pads(&videoscale, NULL, &capsfilter, NULL) == TRUE);
        assert_n(gst_element_link_pads(&capsfilter, NULL, &tee, NULL) == TRUE);
        assert_n(gst_element_link_pads(&tee, NULL, &videoconvert_wl, NULL) == TRUE);
        assert_n(gst_element_link_pads(&videoconvert_wl, NULL, &waylandsink, NULL) == TRUE);
        assert_n(gst_element_link_pads(&tee, NULL, &videoconvert_enc, NULL) == TRUE);
        assert_n(gst_element_link_pads(&videoconvert_enc, NULL, &enc, NULL) == TRUE);
        assert_n(gst_element_link_pads(&enc, NULL, self.jitsibin_sink, "video_sink") == TRUE);

        assert_n(gst_element_sync_state_with_parent(&enc) == TRUE);
        assert_n(gst_element_sync_state_with_parent(&videoconvert_enc) == TRUE);
        assert_n(gst_element_sync_state_with_parent(&waylandsink) == TRUE);
        assert_n(gst_element_sync_state_with_parent(&videoconvert_wl) == TRUE);
        assert_n(gst_element_sync_state_with_parent(&tee) == TRUE);
        assert_n(gst_element_sync_state_with_parent(&capsfilter) == TRUE);
        assert_n(gst_element_sync_state_with_parent(&videoscale) == TRUE);
        assert_n(gst_element_sync_state_with_parent(&dec) == TRUE);

        connected = true;
        PRINT("video connected");
        return;
    } else {
        const auto sink_pad = AutoGstObject(gst_element_get_static_pad(self.jitsibin_sink, "audio_sink"));
        assert_n(sink_pad.get() != NULL);
        assert_n(gst_pad_link(pad, sink_pad.get()) == GST_PAD_LINK_OK);
        connected = true;
        PRINT("audio connected");
        return;
    }
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

auto run() -> bool {
    const auto pipeline = AutoGstObject(gst_pipeline_new(NULL));
    assert_b(pipeline.get() != NULL);

    unwrap_pb_mut(jitsibin_src, add_new_element_to_pipeine(pipeline.get(), "jitsibin"));
    unwrap_pb_mut(jitsibin_sink, add_new_element_to_pipeine(pipeline.get(), "jitsibin"));

    auto context = Context{
        .pipeline      = pipeline.get(),
        .jitsibin_src  = &jitsibin_src,
        .jitsibin_sink = &jitsibin_sink,
    };

    g_signal_connect(&jitsibin_src, "pad-added", G_CALLBACK(jitsibin_pad_added_handler), &context);
    g_signal_connect(&jitsibin_src, "pad-removed", G_CALLBACK(jitsibin_pad_removed_handler), &context);

    g_object_set(&jitsibin_src,
                 "server", "jitsi.local",
                 "room", "src",
                 "nick", "agent-src",
                 "receive-limit", 1,
                 "insecure", TRUE,
                 NULL);

    g_object_set(&jitsibin_sink,
                 "server", "jitsi.local",
                 "room", "sink",
                 "nick", "agent-sink",
                 "force-play", FALSE,
                 "insecure", TRUE,
                 NULL);

    return run_pipeline(pipeline.get());
}
} // namespace

auto main(int argc, char* argv[]) -> int {
    gst_init(&argc, &argv);
    return run() ? 1 : 0;
}
