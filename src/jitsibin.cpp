#include <coop/blocker.hpp>
#include <coop/generator.hpp>
#include <coop/parallel.hpp>
#include <coop/promise.hpp>
#include <coop/single-event.hpp>
#include <coop/task-injector.hpp>
#include <coop/thread.hpp>
#include <coop/timer.hpp>

#include <gst/rtp/gstrtpbasedepayload.h>
#include <gst/rtp/gstrtpdefs.h>
#include <gst/rtp/gstrtphdrext.h>

#include "gstutil/auto-gst-object.hpp"
#include "jitsi/async-websocket.hpp"
#include "jitsi/colibri.hpp"
#include "jitsi/conference.hpp"
#include "jitsi/jingle-handler/jingle.hpp"
#include "jitsi/macros/logger.hpp"
#include "jitsi/util/charconv.hpp"
#include "jitsi/util/pair-table.hpp"
#include "jitsi/util/span.hpp"
#include "jitsi/util/split.hpp"
#include "jitsi/xmpp/elements.hpp"
#include "jitsi/xmpp/negotiator.hpp"
#include "jitsibin.hpp"
#include "macros/autoptr.hpp"
#include "props.hpp"

#define CUTIL_MACROS_PRINT_FUNC(...) LOG_ERROR(logger, __VA_ARGS__)
#include "macros/unwrap.hpp"

#define gst_jitsibin_parent_class parent_class
G_DEFINE_TYPE(GstJitsiBin, gst_jitsibin, GST_TYPE_BIN);

struct RealSelf {
    GstBin*                    bin;
    ws::client::AsyncContext   ws_context;
    JingleHandler*             jingle_handler;
    xmpp::Jid                  jid;
    std::vector<xmpp::Service> extenal_services;

    coop::Runner       runner;
    coop::TaskInjector injector = coop::TaskInjector(runner);
    coop::TaskHandle   connection_task;
    coop::TaskHandle   ws_task;
    std::thread        runner_thread;

    Props props;

    // for unblocking setup
    struct SinkElements {
        GstPad*     sink_pad;  // ghostpad of jitsibin
        GstElement* stub_sink; // fakesink
        GstElement* real_sink; // videopay/audiopay
    };
    SinkElements video_sink_elements;
    SinkElements audio_sink_elements;
};

namespace {
auto logger = Logger("jitsibin");

#define call_vfunc(self, func, ...) \
    GST_BIN_GET_CLASS(self.bin)->func(self.bin, __VA_ARGS__)

declare_autoptr(GstStructure, GstStructure, gst_structure_free);
declare_autoptr(GString, gchar, g_free);

const auto codec_type_to_payloader_name = make_pair_table<CodecType, std::string_view>({
    {CodecType::Opus, "rtpopuspay"},
    {CodecType::H264, "rtph264pay"},
    {CodecType::Vp8, "rtpvp8pay"},
    {CodecType::Vp9, "rtpvp9pay"},
    {CodecType::Av1, "rtpav1pay"},
});

const auto codec_type_to_depayloader_name = make_pair_table<CodecType, std::string_view>({
    {CodecType::Opus, "rtpopusdepay"},
    {CodecType::H264, "rtph264depay"},
    {CodecType::Vp8, "rtpvp8depay"},
    {CodecType::Vp9, "rtpvp9depay"},
    {CodecType::Av1, "rtpav1depay"},
});

const auto codec_type_to_rtp_encoding_name = make_pair_table<CodecType, std::string_view>({
    {CodecType::Opus, "OPUS"},
    {CodecType::H264, "H264"},
    {CodecType::Vp8, "VP8"},
    {CodecType::Vp9, "VP9"},
    {CodecType::Av1, "AV1"},
});

auto set_prop(GObject* obj, const guint id, const GValue* const value, GParamSpec* const spec) -> void {
    const auto jitsibin = GST_JITSIBIN(obj);
    auto&      self     = *jitsibin->real_self;
    self.props.handle_set_prop(id, value, spec);
}

auto get_prop(GObject* obj, const guint id, GValue* const value, GParamSpec* const spec) -> void {
    const auto jitsibin = GST_JITSIBIN(obj);
    auto&      self     = *jitsibin->real_self;
    self.props.handle_get_prop(id, value, spec);
}

auto rtpbin_request_pt_map_handler(GstElement* const /*rtpbin*/, const guint session, const guint pt, const gpointer data) -> GstCaps* {
    auto& self = *std::bit_cast<RealSelf*>(data);
    LOG_DEBUG(logger, "rtpbin request-pt-map session={} pt={}", session, pt);
    const auto& jingle_session = self.jingle_handler->get_session();

    const auto caps = gst_caps_new_simple("application/x-rtp",
                                          "payload", G_TYPE_INT, pt,
                                          NULL);
    for(const auto& codec : jingle_session.codecs) {
        if(codec.tx_pt == int(pt)) {
            unwrap(encoding_name, codec_type_to_rtp_encoding_name.find(codec.type));
            switch(codec.type) {
            case CodecType::Opus: {
                gst_caps_set_simple(caps,
                                    "media", G_TYPE_STRING, "audio",
                                    "encoding-name", G_TYPE_STRING, encoding_name.data(),
                                    "clock-rate", G_TYPE_INT, 48000,
                                    NULL);
                if(const auto ext = jingle_session.audio_hdrext_transport_cc; ext != -1) {
                    const auto name = std::format("extmap-{}", ext);
                    gst_caps_set_simple(caps,
                                        name.data(), G_TYPE_STRING, rtp_hdrext_transport_cc_uri,
                                        NULL);
                }
                if(const auto ext = jingle_session.audio_hdrext_ssrc_audio_level; ext != -1) {
                    const auto name = std::format("extmap-{}", ext);
                    gst_caps_set_simple(caps,
                                        name.data(), G_TYPE_STRING, rtp_hdrext_ssrc_audio_level_uri,
                                        NULL);
                }
                return caps;
            } break;
            case CodecType::H264:
            case CodecType::Vp8:
            case CodecType::Vp9:
            case CodecType::Av1: {
                gst_caps_set_simple(caps,
                                    "media", G_TYPE_STRING, "video",
                                    "encoding-name", G_TYPE_STRING, encoding_name.data(),
                                    "clock-rate", G_TYPE_INT, 90000,
                                    "rtcp-fb-nack-pli", G_TYPE_BOOLEAN, TRUE,
                                    NULL);
                if(const auto ext = jingle_session.video_hdrext_transport_cc; ext != -1) {
                    const auto name = std::format("extmap-{}", ext);
                    gst_caps_set_simple(caps,
                                        name.data(), G_TYPE_STRING, rtp_hdrext_transport_cc_uri,
                                        NULL);
                }
                return caps;
            } break;
            }
        } else if(codec.rtx_pt == int(pt)) {
            gst_caps_set_simple(caps,
                                "media", G_TYPE_STRING, "video",
                                "encoding-name", G_TYPE_STRING, "RTX",
                                "clock-rate", G_TYPE_INT, 90000,
                                "apt", G_TYPE_INT, codec.tx_pt,
                                NULL);
            return caps;
        }
    }
    g_object_unref(caps);
    LOG_WARN(logger, "unknown payload type requested");
    return NULL;
}

auto rtpbin_new_jitterbuffer_handler(GstElement* const /*rtpbin*/, GstElement* const jitterbuffer, const guint session, const guint ssrc, gpointer const data) -> void {
    auto& self = *std::bit_cast<RealSelf*>(data);
    LOG_DEBUG(logger, "rtpbin new-jitterbuffer session={} ssrc={}", session, ssrc);
    const auto& jingle_session = self.jingle_handler->get_session();

    auto source = (const Source*)(nullptr);
    if(const auto i = jingle_session.ssrc_map.find(ssrc); i != jingle_session.ssrc_map.end()) {
        source = &i->second;
    }
    if(source == nullptr) {
        LOG_WARN(logger, "unknown ssrc {}", ssrc);
        for(auto i = jingle_session.ssrc_map.begin(); i != jingle_session.ssrc_map.end(); i = std::next(i)) {
            LOG_DEBUG(logger, "known ssrc {} {}", i->second.ssrc, i->second.participant_id);
        }
        return;
    }
    LOG_DEBUG(logger, "jitterbuffer is for remote source {}", source->participant_id);
    if(source->type != SourceType::Video) {
        return;
    }
    LOG_DEBUG(logger, "enabling RTX");

    g_object_set(jitterbuffer,
                 "do-retransmission", TRUE,
                 "drop-on-latency", TRUE,
                 "latency", self.props.jitterbuffer_latency,
                 NULL);
}

auto aux_handler_create_pt_map(const std::span<const Codec> codecs) -> AutoGstStructure {
    auto pt_map = AutoGstStructure(gst_structure_new_empty("application/x-rtp-pt-map"));
    for(const auto& codec : codecs) {
        if(codec.rtx_pt == -1) {
            continue;
        }
        gst_structure_set(pt_map.get(), std::to_string(codec.tx_pt).data(), G_TYPE_UINT, codec.rtx_pt, NULL);
    }
    return pt_map;
}

auto aux_handler_create_ghost_pad(GstElement* const target, const guint session, const char* const src_or_sink) -> AutoGstObject<GstPad> {
    constexpr auto error_value = nullptr;

    const auto pad_name   = std::format("{}_{}", src_or_sink, session);
    const auto target_pad = AutoGstObject(gst_element_get_static_pad(target, src_or_sink));
    ensure_v(target_pad.get() != NULL);
    const auto pad = gst_ghost_pad_new(pad_name.data(), GST_PAD(target_pad.get()));
    return AutoGstObject(pad);
}

auto rtpbin_request_aux_sender_handler(GstElement* const /*rtpbin*/, const guint session, gpointer const data) -> GstElement* {
    auto& self = *std::bit_cast<RealSelf*>(data);
    LOG_DEBUG(logger, "rtpbin request-aux-sender session={}", session);
    const auto& jingle_session = self.jingle_handler->get_session();

    const auto pt_map   = aux_handler_create_pt_map(jingle_session.codecs);
    const auto ssrc_map = AutoGstStructure(gst_structure_new("application/x-rtp-ssrc-map",
                                                             std::to_string(jingle_session.video_ssrc).data(), G_TYPE_INT, jingle_session.video_rtx_ssrc,
                                                             NULL));

    auto bin        = AutoGstObject(gst_bin_new(NULL));
    auto rtprtxsend = AutoGstObject(gst_element_factory_make("rtprtxsend", NULL));
    ensure(rtprtxsend, "failed to create rtprtxsend");

    g_object_set(rtprtxsend.get(),
                 "payload-type-map", pt_map.get(),
                 "ssrc-map", ssrc_map.get(),
                 NULL);
    gst_bin_add(GST_BIN(bin.get()), rtprtxsend.get());

    auto src_pad = aux_handler_create_ghost_pad(rtprtxsend.get(), session, "src");
    ensure(src_pad);
    auto sink_pad = aux_handler_create_ghost_pad(rtprtxsend.get(), session, "sink");
    ensure(sink_pad);
    ensure(gst_element_add_pad(bin.get(), GST_PAD(src_pad.get())) == TRUE);
    ensure(gst_element_add_pad(bin.get(), GST_PAD(sink_pad.get())) == TRUE);
    return bin.release();
}

auto rtpbin_request_aux_receiver_handler(GstElement* const /*rtpbin*/, const guint session, gpointer const data) -> GstElement* {
    auto& self = *std::bit_cast<RealSelf*>(data);
    LOG_DEBUG(logger, "rtpbin request-aux-receiver session={}", session);
    const auto& jingle_session = self.jingle_handler->get_session();

    const auto pt_map = aux_handler_create_pt_map(jingle_session.codecs);

    auto bin           = AutoGstObject(gst_bin_new(NULL));
    auto rtprtxreceive = AutoGstObject(gst_element_factory_make("rtprtxreceive", NULL));
    ensure(rtprtxreceive, "failed to create rtprtxreceive");

    g_object_set(rtprtxreceive.get(),
                 "payload-type-map", pt_map.get(),
                 NULL);
    gst_bin_add(GST_BIN(bin.get()), rtprtxreceive.get());

    auto src_pad = aux_handler_create_ghost_pad(rtprtxreceive.get(), session, "src");
    ensure(src_pad);
    auto sink_pad = aux_handler_create_ghost_pad(rtprtxreceive.get(), session, "sink");
    ensure(sink_pad);
    ensure(gst_element_add_pad(bin.get(), GST_PAD(src_pad.get())) == TRUE);
    ensure(gst_element_add_pad(bin.get(), GST_PAD(sink_pad.get())) == TRUE);
    return bin.release();
}

auto pay_depay_request_extension_handler(GstRTPBaseDepayload* const /*depay*/, const guint ext_id, const gchar* ext_uri, gpointer const /*data*/) -> GstRTPHeaderExtension* {
    LOG_DEBUG(logger, "(de)payloader extension request ext_id={} ext_uri={}", ext_id, ext_uri);

    auto ext = gst_rtp_header_extension_create_from_uri(ext_uri);
    ensure(ext != NULL);
    gst_rtp_header_extension_set_id(ext, ext_id);
    return ext;
}

auto rtpbin_pad_added_handler(GstElement* const /*rtpbin*/, GstPad* const pad, gpointer const data) -> void {
    auto& self = *std::bit_cast<RealSelf*>(data);
    LOG_DEBUG(logger, "rtpbin pad_added");
    const auto& jingle_session = self.jingle_handler->get_session();

    const auto name_g = AutoGString(gst_object_get_name(GST_OBJECT(pad)));
    const auto name   = std::string_view(name_g.get());
    LOG_DEBUG(logger, "pad name={}", name);
    if(!name.starts_with("recv_rtp_src_0_")) {
        return;
    }

    // get ssrc and pt from pad name
    const auto elms = split(name, "_");
    ensure(elms.size() == 6, "malformed pad name");
    unwrap(ssrc, from_chars<uint32_t>(elms[4]));
    unwrap(pt, from_chars<uint8_t>(elms[5]));

    auto source = (const Source*)(nullptr);
    if(const auto i = jingle_session.ssrc_map.find(ssrc); i != jingle_session.ssrc_map.end()) {
        source = &i->second;
    }

    auto use_fakesink = false;
    if(source == nullptr) {
        // jicofo did not send source-add jingle?
        // we cannot handle this pad since we do not know its format.
        LOG_WARN(logger, "unknown ssrc {}\ninstalling fakesink...", ssrc);
        use_fakesink = true;
    } else if(self.props.last_n == 0) {
        // why jvb send stream while last_n == 0?
        // user probably do not handle this pad.
        LOG_WARN(logger, "unwanted stream found. installing fakesink...");
        use_fakesink = true;
    }
    if(use_fakesink) {
        // add fakesink to prevent broken pipeline
        const auto fakesink = AutoGstObject(gst_element_factory_make("fakesink", NULL));
        ensure(call_vfunc(self, add_element, fakesink.get()) == TRUE);
        const auto fakesink_sink_pad = AutoGstObject(gst_element_get_static_pad(fakesink.get(), "sink"));
        ensure(fakesink_sink_pad.get() != NULL);
        ensure(gst_pad_link(pad, GST_PAD(fakesink_sink_pad.get())) == GST_PAD_LINK_OK);
        ensure(gst_element_sync_state_with_parent(fakesink.get()));
        return;
    }

    LOG_DEBUG(logger, "pad added for remote source {}", source->participant_id);

    // add depayloader
    unwrap(codec, jingle_session.find_codec_by_tx_pt(pt), "cannot find depayloader for such payload type");
    unwrap(depayloader_name, codec_type_to_depayloader_name.find(codec.type));
    const auto depay = AutoGstObject(gst_element_factory_make(depayloader_name.data(), NULL));
    g_object_set(depay.get(),
                 "auto-header-extension", FALSE,
                 NULL);
    g_signal_connect(depay.get(), "request-extension", G_CALLBACK(pay_depay_request_extension_handler), &self);
    ensure(call_vfunc(self, add_element, depay.get()) == TRUE);
    ensure(gst_element_sync_state_with_parent(depay.get()));
    const auto depay_sink_pad = AutoGstObject(gst_element_get_static_pad(depay.get(), "sink"));
    ensure(depay_sink_pad.get() != NULL);
    ensure(gst_pad_link(pad, GST_PAD(depay_sink_pad.get())) == GST_PAD_LINK_OK);

    // expose src pad
    unwrap(encoding_name, codec_type_to_rtp_encoding_name.find(codec.type));
    const auto ghost_pad_name = std::format("{}_{}_{}", source->participant_id, encoding_name.data(), ssrc);

    const auto depay_src_pad = AutoGstObject(gst_element_get_static_pad(depay.get(), "src"));
    ensure(depay_src_pad.get() != NULL);

    const auto ghost_pad = AutoGstObject(gst_ghost_pad_new(ghost_pad_name.data(), depay_src_pad.get()));
    ensure(ghost_pad.get() != NULL);

    ensure(gst_element_add_pad(GST_ELEMENT(self.bin), ghost_pad.get()) == TRUE);

    return;
}

auto construct_sub_pipeline(RealSelf& self) -> bool {
    static auto serial_num     = std::atomic_int(0);
    const auto& jingle_session = self.jingle_handler->get_session();

    // rtpbin
    const auto rtpbin = gst_element_factory_make("rtpbin", "rtpbin");
    ensure(rtpbin != NULL, "failed to create rtpbin");
    g_object_set(rtpbin,
                 "rtp-profile", GST_RTP_PROFILE_SAVPF,
                 "autoremove", TRUE,
                 "do-lost", TRUE,
                 "do-sync-event", TRUE,
                 NULL);
    ensure(call_vfunc(self, add_element, rtpbin) == TRUE);
    g_signal_connect(rtpbin, "request-pt-map", G_CALLBACK(rtpbin_request_pt_map_handler), &self);
    g_signal_connect(rtpbin, "new-jitterbuffer", G_CALLBACK(rtpbin_new_jitterbuffer_handler), &self);
    g_signal_connect(rtpbin, "request-aux-sender", G_CALLBACK(rtpbin_request_aux_sender_handler), &self);
    g_signal_connect(rtpbin, "request-aux-receiver", G_CALLBACK(rtpbin_request_aux_receiver_handler), &self);
    g_signal_connect(rtpbin, "pad-added", G_CALLBACK(rtpbin_pad_added_handler), &self);

    // nicesrc
    const auto nicesrc = gst_element_factory_make("nicesrc", "nicesrc");
    ensure(nicesrc != NULL, "failed to create nicesrc");
    g_object_set(nicesrc,
                 "agent", jingle_session.ice_agent.agent.get(),
                 "stream", jingle_session.ice_agent.stream_id,
                 "component", jingle_session.ice_agent.component_id,
                 NULL);
    ensure(call_vfunc(self, add_element, nicesrc) == TRUE);

    // nicesink
    const auto nicesink = gst_element_factory_make("nicesink", "nicesink");
    ensure(nicesink != NULL, "failed to create nicesink");
    g_object_set(nicesink,
                 "agent", jingle_session.ice_agent.agent.get(),
                 "stream", jingle_session.ice_agent.stream_id,
                 "component", jingle_session.ice_agent.component_id,
                 "sync", FALSE,
                 "async", FALSE,
                 NULL);
    ensure(call_vfunc(self, add_element, nicesink) == TRUE);

    // unique id for dtls enc/dec pair
    const auto dtls_conn_id = std::format("gstjitsimeet-{}", serial_num.fetch_add(1));

    // dtlssrtpenc
    const auto dtlssrtpenc = gst_element_factory_make("dtlssrtpenc", NULL);
    ensure(dtlssrtpenc != NULL, "failed to create dtlssrtpenc");
    g_object_set(dtlssrtpenc,
                 "connection-id", dtls_conn_id.data(),
                 "is-client", TRUE,
                 NULL);
    ensure(call_vfunc(self, add_element, dtlssrtpenc) == TRUE);

    // dtlssrtpdec
    const auto dtlssrtpdec = gst_element_factory_make("dtlssrtpdec", NULL);
    ensure(dtlssrtpdec != NULL, "failed to create dtlssrtpdec");
    g_object_set(dtlssrtpdec,
                 "connection-id", dtls_conn_id.data(),
                 "pem", (jingle_session.dtls_cert_pem + "\n" + jingle_session.dtls_priv_key_pem).data(),
                 NULL);
    ensure(call_vfunc(self, add_element, dtlssrtpdec) == TRUE);

    // audio payloader
    unwrap(audio_codec, jingle_session.find_codec_by_type(self.props.audio_codec_type));
    unwrap(audio_pay_name, codec_type_to_payloader_name.find(self.props.audio_codec_type));
    const auto audio_pay = gst_element_factory_make(audio_pay_name.data(), NULL);
    ensure(audio_pay != NULL, "failed to create audio payloader");
    g_object_set(audio_pay,
                 "pt", audio_codec.tx_pt,
                 "ssrc", jingle_session.audio_ssrc,
                 NULL);
    switch(self.props.audio_codec_type) {
    case CodecType::Opus:
        g_object_set(audio_pay,
                     "min-ptime", 10u * 1000 * 1000, // 10ms
                     NULL);
        break;
    default:
        bail("codec type bug");
    }
    if(g_object_class_find_property(G_OBJECT_GET_CLASS(audio_pay), "auto-header-extension") != NULL) {
        g_object_set(audio_pay,
                     "auto-header-extension", FALSE,
                     NULL);
        g_signal_connect(audio_pay, "request-extension", G_CALLBACK(pay_depay_request_extension_handler), &self);
    }
    ensure(call_vfunc(self, add_element, audio_pay) == TRUE);

    // video payloader
    unwrap(video_codec, jingle_session.find_codec_by_type(self.props.video_codec_type));
    unwrap(video_pay_name, codec_type_to_payloader_name.find(self.props.video_codec_type));
    const auto video_pay = gst_element_factory_make(video_pay_name.data(), NULL);
    ensure(video_pay != NULL, "failed to create video payloader");
    g_object_set(video_pay,
                 "pt", video_codec.tx_pt,
                 "ssrc", jingle_session.video_ssrc,
                 NULL);
    switch(self.props.video_codec_type) {
    case CodecType::H264:
        g_object_set(video_pay,
                     "aggregate-mode", 1, // zero-latency
                     NULL);
        break;
    case CodecType::Vp8:
    case CodecType::Vp9:
        g_object_set(video_pay,
                     "picture-id-mode", 2, // 15-bit
                     NULL);
    case CodecType::Av1:
        break;
    default:
        bail("codec type bug");
    }
    if(g_object_class_find_property(G_OBJECT_GET_CLASS(video_pay), "auto-header-extension") != NULL) {
        g_object_set(video_pay,
                     "auto-header-extension", FALSE,
                     NULL);
        g_signal_connect(video_pay, "request-extension", G_CALLBACK(pay_depay_request_extension_handler), &self);
    }
    ensure(call_vfunc(self, add_element, video_pay) == TRUE);

    // rtpfunnel
    const auto rtpfunnel = gst_element_factory_make("rtpfunnel", NULL);
    ensure(call_vfunc(self, add_element, rtpfunnel) == TRUE);

    // link elements
    // (user) -> audio_pay -> rtpfunnel   -> rtpbin
    // (user) -> video_pay ->
    //           nicesrc   -> dtlssrtpdec ->        -> dtlssrtpenc -> nicesink
    ensure(gst_element_link_pads(audio_pay, NULL, rtpfunnel, NULL) == TRUE);
    ensure(gst_element_link_pads(video_pay, NULL, rtpfunnel, NULL) == TRUE);
    ensure(gst_element_link_pads(rtpfunnel, NULL, rtpbin, "send_rtp_sink_0") == TRUE);
    ensure(gst_element_link_pads(dtlssrtpdec, "rtp_src", rtpbin, "recv_rtp_sink_0") == TRUE);
    ensure(gst_element_link_pads(dtlssrtpdec, "rtcp_src", rtpbin, "recv_rtcp_sink_0") == TRUE);
    ensure(gst_element_link_pads(rtpbin, "send_rtp_src_0", dtlssrtpenc, "rtp_sink_0") == TRUE);
    ensure(gst_element_link_pads(rtpbin, "send_rtcp_src_0", dtlssrtpenc, "rtcp_sink_0") == TRUE);
    ensure(gst_element_link_pads(nicesrc, NULL, dtlssrtpdec, NULL) == TRUE);
    ensure(gst_element_link_pads(dtlssrtpenc, "src", nicesink, "sink") == TRUE);

    self.audio_sink_elements.real_sink = audio_pay;
    self.video_sink_elements.real_sink = video_pay;

    return true;
}

auto replace_stub_sink_with_real_sink(RealSelf& self, RealSelf::SinkElements* elements = nullptr) -> bool;

auto jitsibin_sink_block_callback(GstPad* const pad, GstPadProbeInfo* const /*info*/, gpointer const data) -> GstPadProbeReturn {
    auto& self = *std::bit_cast<RealSelf*>(data);
    if(pad == self.audio_sink_elements.sink_pad) {
        replace_stub_sink_with_real_sink(self, &self.audio_sink_elements);
    } else if(pad == self.video_sink_elements.sink_pad) {
        replace_stub_sink_with_real_sink(self, &self.video_sink_elements);
    } else {
        panic("sink block callback bug");
    }
    return GST_PAD_PROBE_REMOVE;
}

auto replace_stub_sink_with_real_sink(RealSelf& self, RealSelf::SinkElements* const elements) -> bool {
    if(elements == nullptr) {
        // real work must be done in the pad block callback
        gst_pad_add_probe(self.audio_sink_elements.sink_pad, GST_PAD_PROBE_TYPE_BLOCK_DOWNSTREAM, jitsibin_sink_block_callback, &self, NULL);
        gst_pad_add_probe(self.video_sink_elements.sink_pad, GST_PAD_PROBE_TYPE_BLOCK_DOWNSTREAM, jitsibin_sink_block_callback, &self, NULL);
        return true;
    }

    // now the sink pad is in blocked state, safe to modify.

    // remove stub sink
    ensure(gst_element_set_state(elements->stub_sink, GST_STATE_NULL) == GST_STATE_CHANGE_SUCCESS);
    ensure(call_vfunc(self, remove_element, elements->stub_sink) == TRUE);
    elements->stub_sink = nullptr;

    // link real sink to ghostpad
    const auto real_sink_pad = AutoGstObject(gst_element_get_static_pad(elements->real_sink, "sink"));
    ensure(real_sink_pad.get() != NULL);
    ensure(gst_ghost_pad_set_target(GST_GHOST_PAD(elements->sink_pad), real_sink_pad.get()) == TRUE);

    // sync state
    ensure(gst_bin_sync_children_states(self.bin) == TRUE);

    return true;
}

auto setup_stub_pipeline(RealSelf& self) -> bool {
    for(const auto elements : {&self.audio_sink_elements, &self.video_sink_elements}) {
        auto fakesink = gst_element_factory_make("fakesink", NULL);
        ensure(fakesink != NULL, "failed to create fakesink");
        g_object_set(fakesink, "async", FALSE, NULL);
        ensure(call_vfunc(self, add_element, fakesink) == TRUE);
        elements->stub_sink = fakesink;

        // link stub sink to ghostpad
        const auto fakesink_sink_pad = AutoGstObject(gst_element_get_static_pad(fakesink, "sink"));
        ensure(fakesink_sink_pad.get() != NULL);
        ensure(gst_ghost_pad_set_target(GST_GHOST_PAD(elements->sink_pad), fakesink_sink_pad.get()) == TRUE);
    }
    return true;
}

struct XMPPNegotiatorCallbacks : public xmpp::NegotiatorCallbacks {
    ws::client::AsyncContext* ws_context;

    auto send_payload(std::string_view payload) -> void override {
        ensure(ws_context->send(payload));
    }
};

struct ConferenceCallbacks : public conference::ConferenceCallbacks {
    GstJitsiBin*              jitsibin;
    ws::client::AsyncContext* ws_context;
    JingleHandler*            jingle_handler;

    auto on_participant_joined_left(const conference::Participant& participant, const guint signal, const std::string_view debug_label) -> void {
        LOG_DEBUG(logger, "participant {} id={} nick={}", debug_label, participant.participant_id, participant.nick);
        g_signal_emit(jitsibin, signal, 0, participant.participant_id.data(), participant.nick.data());
    }

    auto send_payload(std::string_view payload) -> void override {
        ensure(ws_context->send(payload));
    }

    auto on_jingle_initiate(jingle::Jingle jingle) -> bool override {
        return jingle_handler->on_initiate(std::move(jingle));
    }

    auto on_jingle_add_source(jingle::Jingle jingle) -> bool override {
        return jingle_handler->on_add_source(std::move(jingle));
    }

    auto on_participant_joined(const conference::Participant& participant) -> void override {
        on_participant_joined_left(participant, GST_JITSIBIN_GET_CLASS(jitsibin)->participant_joined_signal, "joined");
    }

    auto on_participant_left(const conference::Participant& participant) -> void override {
        on_participant_joined_left(participant, GST_JITSIBIN_GET_CLASS(jitsibin)->participant_left_signal, "left");
    }

    auto on_mute_state_changed(const conference::Participant& participant, const bool is_audio, const bool new_muted) -> void override {
        LOG_DEBUG(logger, "mute state changed id={} {}={}", participant.participant_id, is_audio ? "audio" : "video", new_muted);
        const auto signal = GST_JITSIBIN_GET_CLASS(jitsibin)->mute_state_changed_signal;
        g_signal_emit(jitsibin, signal, 0,
                      participant.participant_id.data(),
                      is_audio ? TRUE : FALSE,
                      new_muted ? TRUE : FALSE);
    }
};

auto connect_to_conference(RealSelf& self, coop::AtomicEvent& pipeline_ready) -> coop::Async<bool> {
    constexpr auto error_value = false;

    const auto& props = self.props;

    const auto ws_path    = std::format("xmpp-websocket?room={}", props.room_name);
    auto&      ws_context = self.ws_context;
    co_ensure_v(ws_context.init(
        self.injector,
        {
            .address   = props.server_address.data(),
            .path      = ws_path.data(),
            .protocol  = "xmpp",
            .port      = 443,
            .ssl_level = props.secure ? ws::client::SSLLevel::Enable : ws::client::SSLLevel::TrustSelfSigned,
        }));

    co_await coop::run_args(ws_context.process_until_finish()).detach({&self.ws_task});

    auto event = coop::SingleEvent();
    // gain jid from server
    {
        auto callbacks        = XMPPNegotiatorCallbacks();
        callbacks.ws_context  = &ws_context;
        const auto negotiator = xmpp::Negotiator::create(props.server_address, &callbacks);

        ws_context.handler = [&negotiator, &event](const std::span<const std::byte> data) -> coop::Async<void> {
            switch(negotiator->feed_payload(from_span(data))) {
            case xmpp::FeedResult::Continue:
                break;
            case xmpp::FeedResult::Error:
                PANIC();
            case xmpp::FeedResult::Done:
                event.notify();
                break;
            }
            co_return;
        };
        negotiator->start_negotiation();
        co_await event;

        self.jid              = std::move(negotiator->jid);
        self.extenal_services = std::move(negotiator->external_services);
    }

    // join to conference
    auto jingle_handler      = JingleHandler(props.audio_codec_type, props.video_codec_type, self.jid, self.extenal_services, &event);
    auto callbacks           = ConferenceCallbacks();
    callbacks.jitsibin       = GST_JITSIBIN(self.bin);
    callbacks.ws_context     = &ws_context;
    callbacks.jingle_handler = &jingle_handler;
    self.jingle_handler      = &jingle_handler;
    const auto conference    = conference::Conference::create(
        conference::Config{
               .jid              = self.jid,
               .room             = props.room_name,
               .nick             = props.nick,
               .video_codec_type = props.video_codec_type,
               .audio_muted      = false,
               .video_muted      = false,
        },
        &callbacks);
    ws_context.handler = [&conference](const std::span<const std::byte> data) -> coop::Async<void> {
        conference->feed_payload(from_span(data));
        co_return;
    };
    conference->start_negotiation();

    if(props.async_sink) {
        // if there are no participants in the conference, jicofo does not send session-initiate jingle.
        // temporary add fake sinks to pipeline in order to run pipeline immediately.
        co_ensure_v(setup_stub_pipeline(self));
        pipeline_ready.notify();
    }

    co_await event;

    const auto colibri = colibri::Colibri::connect(self.jingle_handler->get_session().initiate_jingle, props.secure);
    co_ensure_v(colibri.get() != nullptr);
    if(props.last_n >= 0) {
        colibri->set_last_n(props.last_n);
    }

    // create pipeline based on the jingle information
    LOG_DEBUG(logger, "creating pipeline");
    co_ensure_v(construct_sub_pipeline(self));

    // expose real pipeline
    if(props.async_sink) {
        // the ghostpad's target is stub sink
        // replace stub sink with real sink
        replace_stub_sink_with_real_sink(self);
    } else {
        // the ghostpad has no target
        // link real sink to ghostpad
        for(const auto elements : {&self.audio_sink_elements, &self.video_sink_elements}) {
            const auto real_sink_pad = AutoGstObject(gst_element_get_static_pad(elements->real_sink, "sink"));
            co_ensure_v(real_sink_pad.get() != NULL);
            co_ensure_v(gst_ghost_pad_set_target(GST_GHOST_PAD(elements->sink_pad), real_sink_pad.get()) == TRUE);
        }
    }

    // send jingle accept
    co_unwrap_v_mut(accept, self.jingle_handler->build_accept_jingle());
    const auto accept_iq = xmpp::elm::iq.clone()
                               .append_attrs({
                                   {"from", self.jid.as_full()},
                                   {"to", conference->config.get_muc_local_focus_jid().as_full()},
                                   {"type", "set"},
                               })
                               .append_children({
                                   jingle::deparse(accept),
                               });

    conference->send_iq(std::move(accept_iq), [](bool success) -> void {
        ASSERT(success, "failed to send accept iq");
    });

    pipeline_ready.notify();

    const auto ping_iq = xmpp::elm::iq.clone()
                             .append_attrs({
                                 {"type", "get"},
                             })
                             .append_children({
                                 xmpp::elm::ping,
                             });
loop:
    co_await coop::sleep(std::chrono::seconds(10));
    conference->send_iq(ping_iq, {});
    goto loop;

    co_return true;
}

auto null_to_ready(RealSelf& self) -> bool {
    ensure(self.props.ensure_required_prop());
    auto pipeline_ready = coop::AtomicEvent();
    self.runner_thread  = std::thread([&self, &pipeline_ready]() {
        self.runner.push_task(std::array{&self.connection_task}, connect_to_conference(self, pipeline_ready));
        self.runner.run();
    });
    pipeline_ready.wait();

    return true;
}

auto ready_to_null(RealSelf& self) -> bool {
    if(self.runner_thread.joinable()) {
        self.injector.inject_task([](RealSelf& self) -> coop::Async<void> {
            self.ws_task.cancel();
            self.connection_task.cancel();
            self.injector.blocker.stop();
            co_return;
        }(self));
        self.runner_thread.join();
    }
    self.ws_context.shutdown();
    return true;
}

auto change_state(GstElement* element, const GstStateChange transition) -> GstStateChangeReturn {
    constexpr auto error_value = GST_STATE_CHANGE_FAILURE;

    const auto jitsibin = GST_JITSIBIN(element);
    auto&      self     = *jitsibin->real_self;

    auto ret = GST_ELEMENT_CLASS(parent_class)->change_state(element, transition);
    ensure_v(ret != GST_STATE_CHANGE_FAILURE);

    switch(transition) {
    case GST_STATE_CHANGE_NULL_TO_READY:
        ensure_v(null_to_ready(self));
        break;
    case GST_STATE_CHANGE_READY_TO_PAUSED:
        ret = GST_STATE_CHANGE_NO_PREROLL;
        break;
    case GST_STATE_CHANGE_PLAYING_TO_PAUSED:
        ret = GST_STATE_CHANGE_NO_PREROLL;
        break;
    case GST_STATE_CHANGE_READY_TO_NULL:
        ensure_v(ready_to_null(self));
        break;
    default:
        break;
    }
    return ret;
}
} // namespace

auto gst_jitsibin_init(GstJitsiBin* jitsibin) -> void {
    jitsibin->real_self      = new RealSelf();
    jitsibin->real_self->bin = &jitsibin->bin;

    auto& self = *jitsibin->real_self;

    // audio sink
    unwrap_mut(jitsibin_audio_sink, gst_ghost_pad_new_no_target("audio_sink", GST_PAD_SINK));
    ensure(gst_element_add_pad(GST_ELEMENT(self.bin), &jitsibin_audio_sink) == TRUE);
    self.audio_sink_elements.sink_pad = &jitsibin_audio_sink;
    // video sink
    unwrap_mut(jitsibin_video_sink, gst_ghost_pad_new_no_target("video_sink", GST_PAD_SINK));
    ensure(gst_element_add_pad(GST_ELEMENT(self.bin), &jitsibin_video_sink) == TRUE);
    self.video_sink_elements.sink_pad = &jitsibin_video_sink;

    return;
}

auto gst_jitsibin_finalize(GObject* object) -> void {
    const auto jitsibin = GST_JITSIBIN(object);
    delete jitsibin->real_self;
    G_OBJECT_CLASS(parent_class)->finalize(object);
}

auto gst_jitsibin_class_init(GstJitsiBinClass* klass) -> void {
    klass->participant_joined_signal = g_signal_new(
        "participant-joined", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_FIRST, 0, NULL, NULL, NULL, G_TYPE_NONE,
        2, G_TYPE_STRING, G_TYPE_STRING);
    klass->participant_left_signal = g_signal_new(
        "participant-left", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_FIRST, 0, NULL, NULL, NULL, G_TYPE_NONE,
        2, G_TYPE_STRING, G_TYPE_STRING);
    klass->mute_state_changed_signal = g_signal_new(
        "mute-state-changed", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_FIRST, 0, NULL, NULL, NULL, G_TYPE_NONE,
        3, G_TYPE_STRING, G_TYPE_BOOLEAN, G_TYPE_BOOLEAN);

    parent_class = g_type_class_peek_parent(klass);

    const auto gobject_class    = (GObjectClass*)(klass);
    gobject_class->set_property = set_prop;
    gobject_class->get_property = get_prop;
    gobject_class->finalize     = gst_jitsibin_finalize;
    Props::install_props(gobject_class);

    const auto element_class    = (GstElementClass*)(klass);
    element_class->change_state = change_state;
    gst_element_class_set_static_metadata(element_class,
                                          "Jitsi Meet Bin",
                                          "Filter/Network/RTP",
                                          "Jitsi Meet Bin",
                                          "mojyack <mojyack@gmail.com>");
}
