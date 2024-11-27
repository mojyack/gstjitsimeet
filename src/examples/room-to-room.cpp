#include <gst/gst.h>
#include <gst/video/gstvideodecoder.h>

#include "../gstutil/auto-gst-object.hpp"
#include "../gstutil/pipeline-helper.hpp"
#include "../macros/autoptr.hpp"
#include "../macros/unwrap.hpp"
#include "helper.hpp"

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

auto jitsibin_pad_added_handler(GstElement* const /*jitsibin*/, GstPad* const pad, gpointer const data) -> void {
    auto& self = *std::bit_cast<Context*>(data);

    const auto name_g = AutoGString(gst_object_get_name(GST_OBJECT(pad)));
    const auto name   = std::string_view(name_g.get());
    line_print("pad added name=", name);

    unwrap(pad_name, parse_jitsibin_pad_name(name));

    auto audio_decoder_name = (const char*)(nullptr);
    auto video_decoder_name = (const char*)(nullptr);
    if(pad_name.codec == "OPUS") {
        audio_decoder_name = "opusdec";
    } else if(pad_name.codec == "H264") {
        video_decoder_name = "avdec_h264";
    } else if(pad_name.codec == "VP8") {
        video_decoder_name = "avdec_vp8";
    } else if(pad_name.codec == "VP9") {
        video_decoder_name = "avdec_vp9";
    } else {
        line_print("unsupported codec: ", pad_name.codec);
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

        unwrap_mut(dec, add_new_element_to_pipeine(self.pipeline, video_decoder_name));
        unwrap_mut(videoscale, add_new_element_to_pipeine(self.pipeline, "videoscale"));
        unwrap_mut(capsfilter, add_new_element_to_pipeine(self.pipeline, "capsfilter"));
        g_object_set(&capsfilter, "caps", caps.get(), NULL);
        unwrap_mut(tee, add_new_element_to_pipeine(self.pipeline, "tee"));
        unwrap_mut(videoconvert_wl, add_new_element_to_pipeine(self.pipeline, "videoconvert"));
        unwrap_mut(waylandsink, add_new_element_to_pipeine(self.pipeline, "waylandsink"));
        unwrap_mut(videoconvert_enc, add_new_element_to_pipeine(self.pipeline, "videoconvert"));
        unwrap_mut(enc, add_new_element_to_pipeine(self.pipeline, "x264enc"));
        const auto dec_sink_pad = AutoGstObject(gst_element_get_static_pad(&dec, "sink"));
        ensure(gst_pad_link(pad, dec_sink_pad.get()) == GST_PAD_LINK_OK);
        ensure(gst_element_link_pads(&dec, NULL, &videoscale, NULL) == TRUE);
        ensure(gst_element_link_pads(&videoscale, NULL, &capsfilter, NULL) == TRUE);
        ensure(gst_element_link_pads(&capsfilter, NULL, &tee, NULL) == TRUE);
        ensure(gst_element_link_pads(&tee, NULL, &videoconvert_wl, NULL) == TRUE);
        ensure(gst_element_link_pads(&videoconvert_wl, NULL, &waylandsink, NULL) == TRUE);
        ensure(gst_element_link_pads(&tee, NULL, &videoconvert_enc, NULL) == TRUE);
        ensure(gst_element_link_pads(&videoconvert_enc, NULL, &enc, NULL) == TRUE);
        ensure(gst_element_link_pads(&enc, NULL, self.jitsibin_sink, "video_sink") == TRUE);

        ensure(gst_element_sync_state_with_parent(&enc) == TRUE);
        ensure(gst_element_sync_state_with_parent(&videoconvert_enc) == TRUE);
        ensure(gst_element_sync_state_with_parent(&waylandsink) == TRUE);
        ensure(gst_element_sync_state_with_parent(&videoconvert_wl) == TRUE);
        ensure(gst_element_sync_state_with_parent(&tee) == TRUE);
        ensure(gst_element_sync_state_with_parent(&capsfilter) == TRUE);
        ensure(gst_element_sync_state_with_parent(&videoscale) == TRUE);
        ensure(gst_element_sync_state_with_parent(&dec) == TRUE);

        connected = true;
        line_print("video connected");
        return;
    } else {
        const auto sink_pad = AutoGstObject(gst_element_get_static_pad(self.jitsibin_sink, "audio_sink"));
        ensure(sink_pad.get() != NULL);
        ensure(gst_pad_link(pad, sink_pad.get()) == GST_PAD_LINK_OK);
        connected = true;
        line_print("audio connected");
        return;
    }
}

auto jitsibin_pad_removed_handler(GstElement* const /*jitisbin*/, GstPad* const pad, gpointer const /*data*/) -> void {
    const auto name_g = AutoGString(gst_object_get_name(GST_OBJECT(pad)));
    const auto name   = std::string_view(name_g.get());
    line_print("pad removed name=", name);
}

auto run() -> bool {
    const auto pipeline = AutoGstObject(gst_pipeline_new(NULL));
    ensure(pipeline.get() != NULL);

    unwrap_mut(jitsibin_src, add_new_element_to_pipeine(pipeline.get(), "jitsibin"));
    unwrap_mut(jitsibin_sink, add_new_element_to_pipeine(pipeline.get(), "jitsibin"));

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
