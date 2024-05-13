#include <chrono>
#include <thread>

#include <gst/gst.h>
#include <gst/video/gstvideodecoder.h>

#include "auto-gst-object.hpp"
#include "jitsi/autoptr.hpp"
#include "jitsi/unwrap.hpp"

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
    // sink 1
    GstElement* fakesink;
    // sink 2
    GstElement* videoconvert;
    GstElement* waylandsink;
};

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

auto switch_fake_to_wayland(Context& self) -> bool {
    // remove old elements
    assert_b(gst_element_set_state(self.fakesink, GST_STATE_NULL) == GST_STATE_CHANGE_SUCCESS);
    assert_b(gst_bin_remove(GST_BIN(self.pipeline), self.fakesink) == TRUE);
    self.fakesink = nullptr;
    // create new elements
    unwrap_pb_mut(videoconvert, add_new_element_to_pipeine(self.pipeline, "videoconvert"));
    unwrap_pb_mut(waylandsink, add_new_element_to_pipeine(self.pipeline, "waylandsink"));
    self.videoconvert = &videoconvert;
    self.waylandsink  = &waylandsink;
    assert_b(gst_element_link_pads(self.videotestsrc, NULL, &videoconvert, NULL) == TRUE);
    assert_b(gst_element_link_pads(&videoconvert, NULL, &waylandsink, NULL) == TRUE);
    return true;
}

auto switch_wayland_to_fake(Context& self) -> bool {
    // remove old elements
    assert_b(gst_element_set_state(self.videoconvert, GST_STATE_NULL) == GST_STATE_CHANGE_SUCCESS);
    assert_b(gst_bin_remove(GST_BIN(self.pipeline), self.videoconvert) == TRUE);
    self.videoconvert = nullptr;
    assert_b(gst_element_set_state(self.waylandsink, GST_STATE_NULL) == GST_STATE_CHANGE_SUCCESS);
    assert_b(gst_bin_remove(GST_BIN(self.pipeline), self.waylandsink) == TRUE);
    self.waylandsink = nullptr;
    // create new elements
    unwrap_pb_mut(fakesink, add_new_element_to_pipeine(self.pipeline, "fakesink"));
    self.fakesink = &fakesink;
    assert_b(gst_element_link_pads(self.videotestsrc, NULL, &fakesink, NULL) == TRUE);
    return true;
}
auto pad_block_callback(GstPad* const /*pad*/, GstPadProbeInfo* const /*info*/, gpointer const data) -> GstPadProbeReturn {
    auto& self = *std::bit_cast<Context*>(data);
    PRINT("blocked");
    if(self.fakesink != nullptr) {
        switch_fake_to_wayland(self);
    } else {
        switch_wayland_to_fake(self);
    }
    PRINT("unblocking");
    return GST_PAD_PROBE_REMOVE;
}

auto run_dynamic_switch_example() -> bool {
    const auto pipeline = AutoGstObject(gst_pipeline_new(NULL));
    assert_b(pipeline.get() != NULL);

    unwrap_pb_mut(videotestsrc, add_new_element_to_pipeine(pipeline.get(), "videotestsrc"));
    unwrap_pb_mut(fakesink, add_new_element_to_pipeine(pipeline.get(), "fakesink"));

    g_object_set(&videotestsrc,
                 "is-live", TRUE,
                 NULL);

    g_object_set(&fakesink,
                 "async", FALSE,
                 NULL);

    auto context = Context{
        .pipeline     = pipeline.get(),
        .videotestsrc = &videotestsrc,
        .fakesink     = &fakesink,
        .videoconvert = nullptr,
        .waylandsink  = nullptr,
    };

    auto switcher = std::thread([&videotestsrc, &context]() -> bool {
        const auto src_pad = AutoGstObject(gst_element_get_static_pad(&videotestsrc, "src"));
        assert_b(src_pad.get() != NULL);
        while(true) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            PRINT("switch");
            gst_pad_add_probe(src_pad.get(), GST_PAD_PROBE_TYPE_BLOCK_DOWNSTREAM, pad_block_callback, &context, NULL);
        }
        return true;
    });

    assert_b(gst_element_link_pads(&videotestsrc, NULL, &fakesink, NULL) == TRUE);

    auto ret = run_pipeline(pipeline.get());
    switcher.join();
    return ret;
}
} // namespace

auto main(int argc, char* argv[]) -> int {
    gst_init(&argc, &argv);
    return run_dynamic_switch_example() ? 1 : 0;
}
