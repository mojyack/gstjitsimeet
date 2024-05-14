#include <span>
#include <thread>

#include <gst/rtp/gstrtpbasedepayload.h>
#include <gst/rtp/gstrtpdefs.h>
#include <gst/rtp/gstrtphdrext.h>

#include "auto-gst-object.hpp"
#include "jitsi/autoptr.hpp"
#include "jitsi/colibri.hpp"
#include "jitsi/conference.hpp"
#include "jitsi/jingle-handler/jingle.hpp"
#include "jitsi/unwrap.hpp"
#include "jitsi/util/charconv.hpp"
#include "jitsi/util/event.hpp"
#include "jitsi/util/misc.hpp"
#include "jitsi/websocket.hpp"
#include "jitsi/xmpp/elements.hpp"
#include "jitsi/xmpp/negotiator.hpp"
#include "jitsibin.hpp"
#include "props.hpp"

#define GST_CAT_DEFAULT jitsibin_debug

#define gst_jitsibin_parent_class parent_class
G_DEFINE_TYPE(GstJitsiBin, gst_jitsibin, GST_TYPE_BIN);

struct RealSelf {
    GstBin*                                          bin;
    ws::Connection*                                  ws_conn;
    std::unique_ptr<JingleHandler>                   jingle_handler;
    std::unique_ptr<conference::ConferenceCallbacks> conference_callbacks;
    std::unique_ptr<conference::Conference>          conference;
    std::unique_ptr<colibri::Colibri>                colibri;
    std::thread                                      pinger;
    xmpp::Jid                                        jid;
    std::vector<xmpp::Service>                       extenal_services;
    Event                                            session_initiate_jingle_arrived_event;

    Props props;

    // for unblocking setup
    struct SinkElements {
        GstPad*     sink_pad;  // ghostpad of jitsibin
        GstElement* stub_sink; // fakesink
        GstElement* real_sink; // videopay/audiopay
    };
    SinkElements video_sink_elements;
    SinkElements audio_sink_elements;
    std::thread  session_initiate_jingle_wait_thread;
};

namespace {
GST_DEBUG_CATEGORY_STATIC(jitsibin_debug);

#define call_vfunc(self, func, ...) \
    GST_BIN_GET_CLASS(self.bin)->func(self.bin, __VA_ARGS__)

declare_autoptr(GstStructure, GstStructure, gst_structure_free);
declare_autoptr(GString, gchar, g_free);

const auto codec_type_to_payloader_name = make_str_table<CodecType>({
    {CodecType::Opus, "rtpopuspay"},
    {CodecType::H264, "rtph264pay"},
    {CodecType::Vp8, "rtpvp8pay"},
    {CodecType::Vp9, "rtpvp9pay"},
});

const auto codec_type_to_depayloader_name = make_str_table<CodecType>({
    {CodecType::Opus, "rtpopusdepay"},
    {CodecType::H264, "rtph264depay"},
    {CodecType::Vp8, "rtpvp8depay"},
    {CodecType::Vp9, "rtpvp9depay"},
});

const auto codec_type_to_rtp_encoding_name = make_str_table<CodecType>({
    {CodecType::Opus, "OPUS"},
    {CodecType::H264, "H264"},
    {CodecType::Vp8, "VP8"},
    {CodecType::Vp9, "VP9"},
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

auto rtpbin_request_pt_map_handler(GstElement* const rtpbin, const guint session, const guint pt, const gpointer data) -> GstCaps* {
    auto& self = *std::bit_cast<RealSelf*>(data);
    if(self.props.verbose) {
        PRINT("rtpbin request-pt-map session=", session, " pt=", pt);
    }
    const auto& jingle_session = self.jingle_handler->get_session();

    const auto caps = gst_caps_new_simple("application/x-rtp",
                                          "payload", G_TYPE_INT, pt,
                                          NULL);
    for(const auto& codec : jingle_session.codecs) {
        if(codec.tx_pt == int(pt)) {
            unwrap_pp(encoding_name, codec_type_to_rtp_encoding_name.find(codec.type));
            switch(codec.type) {
            case CodecType::Opus: {
                gst_caps_set_simple(caps,
                                    "media", G_TYPE_STRING, "audio",
                                    "encoding-name", G_TYPE_STRING, encoding_name.second,
                                    "clock-rate", G_TYPE_INT, 48000,
                                    NULL);
                if(const auto ext = jingle_session.audio_hdrext_transport_cc; ext != -1) {
                    const auto name = build_string("extmap-", ext);
                    gst_caps_set_simple(caps,
                                        name.data(), G_TYPE_STRING, rtp_hdrext_transport_cc_uri,
                                        NULL);
                }
                if(const auto ext = jingle_session.audio_hdrext_ssrc_audio_level; ext != -1) {
                    const auto name = build_string("extmap-", ext);
                    gst_caps_set_simple(caps,
                                        name.data(), G_TYPE_STRING, rtp_hdrext_ssrc_audio_level_uri,
                                        NULL);
                }
                return caps;
            } break;
            case CodecType::H264:
            case CodecType::Vp8:
            case CodecType::Vp9: {
                gst_caps_set_simple(caps,
                                    "media", G_TYPE_STRING, "video",
                                    "encoding-name", G_TYPE_STRING, encoding_name.second,
                                    "clock-rate", G_TYPE_INT, 90000,
                                    "rtcp-fb-nack-pli", G_TYPE_BOOLEAN, TRUE,
                                    NULL);
                if(const auto ext = jingle_session.video_hdrext_transport_cc; ext != -1) {
                    const auto name = build_string("extmap-", ext);
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
    WARN("unknown payload type requested");
    return NULL;
}

auto rtpbin_new_jitterbuffer_handler(GstElement* const rtpbin, GstElement* const jitterbuffer, const guint session, const guint ssrc, gpointer const data) -> void {
    auto& self = *std::bit_cast<RealSelf*>(data);
    if(self.props.verbose) {
        PRINT("rtpbin new-jitterbuffer session=", session, " ssrc=", ssrc);
    }
    const auto& jingle_session = self.jingle_handler->get_session();

    auto source = (const Source*)(nullptr);
    if(const auto i = jingle_session.ssrc_map.find(ssrc); i != jingle_session.ssrc_map.end()) {
        source = &i->second;
    }
    if(source == nullptr) {
        WARN("unknown ssrc");
        if(self.props.verbose) {
            for(auto i = jingle_session.ssrc_map.begin(); i != jingle_session.ssrc_map.end(); i = std::next(i)) {
                WARN("known ssrc: ", i->second.ssrc, " ", i->second.participant_id);
            }
        }
        return;
    }
    if(self.props.verbose) {
        PRINT("jitterbuffer is for remote source ", source->participant_id);
    }
    if(source->type != SourceType::Video) {
        return;
    }
    if(self.props.verbose) {
        PRINT("enabling RTX");
    }

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
    const auto pad_name   = build_string(src_or_sink, "_", session);
    const auto target_pad = AutoGstObject(gst_element_get_static_pad(target, src_or_sink));
    assert_p(target_pad.get() != NULL);
    const auto pad = gst_ghost_pad_new(pad_name.data(), GST_PAD(target_pad.get()));
    return AutoGstObject(pad);
}

auto rtpbin_request_aux_sender_handler(GstElement* const rtpbin, const guint session, gpointer const data) -> GstElement* {
    auto& self = *std::bit_cast<RealSelf*>(data);
    if(self.props.verbose) {
        PRINT("rtpbin request-aux-sender session=", session);
    }
    const auto& jingle_session = self.jingle_handler->get_session();

    const auto pt_map   = aux_handler_create_pt_map(jingle_session.codecs);
    const auto ssrc_map = AutoGstStructure(gst_structure_new("application/x-rtp-ssrc-map",
                                                             std::to_string(jingle_session.video_ssrc).data(), G_TYPE_INT, jingle_session.video_rtx_ssrc,
                                                             NULL));

    auto bin        = AutoGstObject(gst_bin_new(NULL));
    auto rtprtxsend = AutoGstObject(gst_element_factory_make("rtprtxsend", NULL));
    assert_p(rtprtxsend, "failed to create rtprtxsend");

    g_object_set(rtprtxsend.get(),
                 "payload-type-map", pt_map.get(),
                 "ssrc-map", ssrc_map.get(),
                 NULL);
    gst_bin_add(GST_BIN(bin.get()), rtprtxsend.get());

    auto src_pad = aux_handler_create_ghost_pad(rtprtxsend.get(), session, "src");
    assert_p(src_pad);
    auto sink_pad = aux_handler_create_ghost_pad(rtprtxsend.get(), session, "sink");
    assert_p(sink_pad);
    assert_p(gst_element_add_pad(bin.get(), GST_PAD(src_pad.get())) == TRUE);
    assert_p(gst_element_add_pad(bin.get(), GST_PAD(sink_pad.get())) == TRUE);
    return bin.release();
}

auto rtpbin_request_aux_receiver_handler(GstElement* const rtpbin, const guint session, gpointer const data) -> GstElement* {
    auto& self = *std::bit_cast<RealSelf*>(data);
    if(self.props.verbose) {
        PRINT("rtpbin request-aux-receiver session=", session);
    }
    const auto& jingle_session = self.jingle_handler->get_session();

    const auto pt_map = aux_handler_create_pt_map(jingle_session.codecs);

    auto bin           = AutoGstObject(gst_bin_new(NULL));
    auto rtprtxreceive = AutoGstObject(gst_element_factory_make("rtprtxreceive", NULL));
    assert_p(rtprtxreceive, "failed to create rtprtxreceive");

    g_object_set(rtprtxreceive.get(),
                 "payload-type-map", pt_map.get(),
                 NULL);
    gst_bin_add(GST_BIN(bin.get()), rtprtxreceive.get());

    auto src_pad = aux_handler_create_ghost_pad(rtprtxreceive.get(), session, "src");
    assert_p(src_pad);
    auto sink_pad = aux_handler_create_ghost_pad(rtprtxreceive.get(), session, "sink");
    assert_p(sink_pad);
    assert_p(gst_element_add_pad(bin.get(), GST_PAD(src_pad.get())) == TRUE);
    assert_p(gst_element_add_pad(bin.get(), GST_PAD(sink_pad.get())) == TRUE);
    return bin.release();
}

auto pay_depay_request_extension_handler(GstRTPBaseDepayload* depay, const guint ext_id, const gchar* ext_uri, gpointer const data) -> GstRTPHeaderExtension* {
    auto& self = *std::bit_cast<RealSelf*>(data);
    if(self.props.verbose) {
        PRINT("(de)payloader extension request ext_id=", ext_id, " ext_uri=", ext_uri);
    }

    auto ext = gst_rtp_header_extension_create_from_uri(ext_uri);
    assert_p(ext != NULL);
    gst_rtp_header_extension_set_id(ext, ext_id);
    return ext;
}

auto rtpbin_pad_added_handler(GstElement* const rtpbin, GstPad* const pad, gpointer const data) -> void {
    auto& self = *std::bit_cast<RealSelf*>(data);
    if(self.props.verbose) {
        PRINT("rtpbin pad_added");
    }
    const auto& jingle_session = self.jingle_handler->get_session();

    const auto name_g = AutoGString(gst_object_get_name(GST_OBJECT(pad)));
    const auto name   = std::string_view(name_g.get());
    if(self.props.verbose) {
        PRINT("pad name=", name);
    }
    if(!name.starts_with("recv_rtp_src_0_")) {
        return;
    }

    // get ssrc and pt from pad name
    const auto elms = split(name, "_");
    assert_n(elms.size() == 6, "malformed pad name");
    unwrap_on(ssrc, from_chars<uint32_t>(elms[4]));
    unwrap_on(pt, from_chars<uint8_t>(elms[5]));

    auto source = (const Source*)(nullptr);
    if(const auto i = jingle_session.ssrc_map.find(ssrc); i != jingle_session.ssrc_map.end()) {
        source = &i->second;
    }
    assert_n(source != nullptr, "unknown ssrc");
    if(self.props.verbose) {
        PRINT("pad added for remote source ", source->participant_id);
    }

    // add depayloader
    unwrap_pn(codec, jingle_session.find_codec_by_tx_pt(pt), "cannot find depayloader for such payload type");
    unwrap_pn(depayloader_name, codec_type_to_depayloader_name.find(codec.type));
    const auto depay = AutoGstObject(gst_element_factory_make(depayloader_name.second, NULL));
    g_object_set(depay.get(),
                 "auto-header-extension", FALSE,
                 NULL);
    g_signal_connect(depay.get(), "request-extension", G_CALLBACK(pay_depay_request_extension_handler), &self);
    assert_n(call_vfunc(self, add_element, depay.get()) == TRUE);
    assert_n(gst_element_sync_state_with_parent(depay.get()));
    const auto depay_sink_pad = AutoGstObject(gst_element_get_static_pad(depay.get(), "sink"));
    assert_n(depay_sink_pad.get() != NULL);
    assert_n(gst_pad_link(pad, GST_PAD(depay_sink_pad.get())) == GST_PAD_LINK_OK);

    if(self.props.last_n == 0) {
        // we should not reach here
        // why jvb send stream while last_n == 0?
        // user probably do not handle this pad, so add fakesink to prevent broken pipeline
        const auto fakesink = AutoGstObject(gst_element_factory_make("fakesink", NULL));
        assert_n(call_vfunc(self, add_element, fakesink.get()) == TRUE);
        assert_n(gst_element_sync_state_with_parent(fakesink.get()));
        const auto depay_src_pad = AutoGstObject(gst_element_get_static_pad(depay.get(), "src"));
        assert_n(depay_src_pad.get() != NULL);
        const auto fakesink_sink_pad = AutoGstObject(gst_element_get_static_pad(fakesink.get(), "sink"));
        assert_n(fakesink_sink_pad.get() != NULL);
        assert_n(gst_pad_link(GST_PAD(depay_src_pad.get()), GST_PAD(fakesink_sink_pad.get())) == GST_PAD_LINK_OK);

        return;
    }

    // expose src pad
    unwrap_pn(encoding_name, codec_type_to_rtp_encoding_name.find(codec.type));
    const auto ghost_pad_name = build_string(source->participant_id, "_", encoding_name.second, "_", ssrc);

    const auto depay_src_pad = AutoGstObject(gst_element_get_static_pad(depay.get(), "src"));
    assert_n(depay_src_pad.get() != NULL);

    const auto ghost_pad = AutoGstObject(gst_ghost_pad_new(ghost_pad_name.data(), depay_src_pad.get()));
    assert_n(ghost_pad.get() != NULL);

    assert_n(gst_element_add_pad(GST_ELEMENT(self.bin), ghost_pad.get()) == TRUE);

    return;
}

auto construct_sub_pipeline(RealSelf& self) -> bool {
    static auto serial_num     = std::atomic_int(0);
    const auto& jingle_session = self.jingle_handler->get_session();

    // rtpbin
    const auto rtpbin = gst_element_factory_make("rtpbin", "rtpbin");
    assert_b(rtpbin != NULL, "failed to create rtpbin");
    g_object_set(rtpbin,
                 "rtp-profile", GST_RTP_PROFILE_SAVPF,
                 "autoremove", TRUE,
                 "do-lost", TRUE,
                 "do-sync-event", TRUE,
                 NULL);
    assert_b(call_vfunc(self, add_element, rtpbin) == TRUE);
    g_signal_connect(rtpbin, "request-pt-map", G_CALLBACK(rtpbin_request_pt_map_handler), &self);
    g_signal_connect(rtpbin, "new-jitterbuffer", G_CALLBACK(rtpbin_new_jitterbuffer_handler), &self);
    g_signal_connect(rtpbin, "request-aux-sender", G_CALLBACK(rtpbin_request_aux_sender_handler), &self);
    g_signal_connect(rtpbin, "request-aux-receiver", G_CALLBACK(rtpbin_request_aux_receiver_handler), &self);
    g_signal_connect(rtpbin, "pad-added", G_CALLBACK(rtpbin_pad_added_handler), &self);

    // nicesrc
    const auto nicesrc = gst_element_factory_make("nicesrc", "nicesrc");
    assert_b(nicesrc != NULL, "failed to create nicesrc");
    g_object_set(nicesrc,
                 "agent", jingle_session.ice_agent.agent.get(),
                 "stream", jingle_session.ice_agent.stream_id,
                 "component", jingle_session.ice_agent.component_id,
                 NULL);
    assert_b(call_vfunc(self, add_element, nicesrc) == TRUE);

    // nicesink
    const auto nicesink = gst_element_factory_make("nicesink", "nicesink");
    assert_b(nicesink != NULL, "failed to create nicesink");
    g_object_set(nicesink,
                 "agent", jingle_session.ice_agent.agent.get(),
                 "stream", jingle_session.ice_agent.stream_id,
                 "component", jingle_session.ice_agent.component_id,
                 NULL);
    assert_b(call_vfunc(self, add_element, nicesink) == TRUE);

    // unique id for dtls enc/dec pair
    const auto dtls_conn_id = std::string("gstjitsimeet-") + std::to_string(serial_num.fetch_add(1));

    // dtlssrtpenc
    const auto dtlssrtpenc = gst_element_factory_make("dtlssrtpenc", NULL);
    assert_b(dtlssrtpenc != NULL, "failed to create dtlssrtpenc");
    g_object_set(dtlssrtpenc,
                 "connection-id", dtls_conn_id.data(),
                 "is-client", TRUE,
                 NULL);
    assert_b(call_vfunc(self, add_element, dtlssrtpenc) == TRUE);

    // dtlssrtpdec
    const auto dtlssrtpdec = gst_element_factory_make("dtlssrtpdec", NULL);
    assert_b(dtlssrtpdec != NULL, "failed to create dtlssrtpdec");
    g_object_set(dtlssrtpdec,
                 "connection-id", dtls_conn_id.data(),
                 "pem", (jingle_session.dtls_cert_pem + "\n" + jingle_session.dtls_priv_key_pem).data(),
                 NULL);
    assert_b(call_vfunc(self, add_element, dtlssrtpdec) == TRUE);

    // audio payloader
    unwrap_pb(audio_codec, jingle_session.find_codec_by_type(self.props.audio_codec_type));
    unwrap_pb(audio_pay_name, codec_type_to_payloader_name.find(self.props.audio_codec_type));
    const auto audio_pay = gst_element_factory_make(audio_pay_name.second, NULL);
    assert_b(audio_pay != NULL, "failed to create audio payloader");
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
        WARN("codec type bug");
        break;
    }
    if(g_object_class_find_property(G_OBJECT_GET_CLASS(audio_pay), "auto-header-extension") != NULL) {
        g_object_set(audio_pay,
                     "auto-header-extension", FALSE,
                     NULL);
        g_signal_connect(audio_pay, "request-extension", G_CALLBACK(pay_depay_request_extension_handler), &self);
    }
    assert_b(call_vfunc(self, add_element, audio_pay) == TRUE);

    // video payloader
    unwrap_pb(video_codec, jingle_session.find_codec_by_type(self.props.video_codec_type));
    unwrap_pb(video_pay_name, codec_type_to_payloader_name.find(self.props.video_codec_type));
    const auto video_pay = gst_element_factory_make(video_pay_name.second, NULL);
    assert_b(video_pay != NULL, "failed to create video payloader");
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
        break;
    default:
        WARN("codec type bug");
        break;
    }
    if(g_object_class_find_property(G_OBJECT_GET_CLASS(video_pay), "auto-header-extension") != NULL) {
        g_object_set(video_pay,
                     "auto-header-extension", FALSE,
                     NULL);
        g_signal_connect(video_pay, "request-extension", G_CALLBACK(pay_depay_request_extension_handler), &self);
    }
    assert_b(call_vfunc(self, add_element, video_pay) == TRUE);

    // rtpfunnel
    const auto rtpfunnel = gst_element_factory_make("rtpfunnel", NULL);
    assert_b(call_vfunc(self, add_element, rtpfunnel) == TRUE);

    // link elements
    // (user) -> audio_pay -> rtpfunnel   -> rtpbin
    // (user) -> video_pay ->
    //           nicesrc   -> dtlssrtpdec ->        -> dtlssrtpenc -> nicesink
    assert_b(gst_element_link_pads(audio_pay, NULL, rtpfunnel, NULL) == TRUE);
    assert_b(gst_element_link_pads(video_pay, NULL, rtpfunnel, NULL) == TRUE);
    assert_b(gst_element_link_pads(rtpfunnel, NULL, rtpbin, "send_rtp_sink_0") == TRUE);
    assert_b(gst_element_link_pads(dtlssrtpdec, "rtp_src", rtpbin, "recv_rtp_sink_0") == TRUE);
    assert_b(gst_element_link_pads(dtlssrtpdec, "rtcp_src", rtpbin, "recv_rtcp_sink_0") == TRUE);
    assert_b(gst_element_link_pads(rtpbin, "send_rtp_src_0", dtlssrtpenc, "rtp_sink_0") == TRUE);
    assert_b(gst_element_link_pads(rtpbin, "send_rtcp_src_0", dtlssrtpenc, "rtcp_sink_0") == TRUE);
    assert_b(gst_element_link_pads(nicesrc, NULL, dtlssrtpdec, NULL) == TRUE);
    assert_b(gst_element_link_pads(dtlssrtpenc, "src", nicesink, "sink") == TRUE);

    self.audio_sink_elements.real_sink = audio_pay;
    self.video_sink_elements.real_sink = video_pay;

    return true;
}

auto replace_stub_sink_with_real_sink(RealSelf& self, RealSelf::SinkElements* elements = nullptr) -> bool;

auto jitsibin_sink_block_callback(GstPad* const pad, GstPadProbeInfo* const info, gpointer const data) -> GstPadProbeReturn {
    auto& self = *std::bit_cast<RealSelf*>(data);
    if(pad == self.audio_sink_elements.sink_pad) {
        replace_stub_sink_with_real_sink(self, &self.audio_sink_elements);
    } else if(pad == self.video_sink_elements.sink_pad) {
        replace_stub_sink_with_real_sink(self, &self.video_sink_elements);
    } else {
        PANIC("sink block callback bug");
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
    assert_b(gst_element_set_state(elements->stub_sink, GST_STATE_NULL) == GST_STATE_CHANGE_SUCCESS);
    assert_b(call_vfunc(self, remove_element, elements->stub_sink) == TRUE);
    elements->stub_sink = nullptr;

    // link real sink to ghostpad
    const auto real_sink_pad = AutoGstObject(gst_element_get_static_pad(elements->real_sink, "sink"));
    assert_b(real_sink_pad.get() != NULL);
    assert_b(gst_ghost_pad_set_target(GST_GHOST_PAD(elements->sink_pad), real_sink_pad.get()) == TRUE);

    // sync state
    assert_b(gst_bin_sync_children_states(self.bin) == TRUE);

    return true;
}

auto setup_stub_pipeline(RealSelf& self) -> bool {
    for(auto i = 0; i < 2; i += 1) {
        const auto elements = i == 0 ? &self.audio_sink_elements : &self.video_sink_elements;
        auto       fakesink = gst_element_factory_make("fakesink", NULL);
        assert_b(fakesink != NULL, "failed to create fakesink");
        g_object_set(fakesink,
                     "async", FALSE,
                     NULL);
        assert_b(call_vfunc(self, add_element, fakesink) == TRUE);
        elements->stub_sink = fakesink;

        // link stub sink to ghostpad
        const auto fakesink_sink_pad = AutoGstObject(gst_element_get_static_pad(fakesink, "sink"));
        assert_b(fakesink_sink_pad.get() != NULL);
        assert_b(gst_ghost_pad_set_target(GST_GHOST_PAD(elements->sink_pad), fakesink_sink_pad.get()) == TRUE);
    }

    return true;
}

auto wait_for_jingle_and_setup_pipeline(RealSelf& self) -> bool {
    // wait for jingle initiation
    self.session_initiate_jingle_arrived_event.wait();

    self.colibri = colibri::Colibri::connect(self.jingle_handler->get_session().initiate_jingle, self.props.secure);
    assert_b(self.colibri.get() != nullptr);
    if(self.props.last_n >= 0) {
        self.colibri->set_last_n(self.props.last_n);
    }

    // create pipeline based on the jingle information
    if(self.props.verbose) {
        PRINT("creating pipeline");
    }
    assert_b(construct_sub_pipeline(self));

    // expose real pipeline
    if(self.props.async_sink) {
        // we are in sub thread
        // the ghostpad's target is stub sink
        // replace stub sink with real sink
        replace_stub_sink_with_real_sink(self);
    } else {
        // we are in main thread
        // the ghostpad has no target
        // link real sink to ghostpad
        for(auto i = 0; i < 2; i += 1) {
            const auto* elements      = i == 0 ? &self.audio_sink_elements : &self.video_sink_elements;
            const auto  real_sink_pad = AutoGstObject(gst_element_get_static_pad(elements->real_sink, "sink"));
            assert_b(real_sink_pad.get() != NULL);
            assert_b(gst_ghost_pad_set_target(GST_GHOST_PAD(elements->sink_pad), real_sink_pad.get()) == TRUE);
        }
    }

    // send jingle accept
    unwrap_ob_mut(accept, self.jingle_handler->build_accept_jingle());
    const auto accept_iq = xmpp::elm::iq.clone()
                               .append_attrs({
                                   {"from", self.jid.as_full()},
                                   {"to", self.conference->config.get_muc_local_focus_jid().as_full()},
                                   {"type", "set"},
                               })
                               .append_children({
                                   jingle::deparse(accept),
                               });

    self.conference->send_iq(std::move(accept_iq), [](bool success) -> void {
        DYN_ASSERT(success, "failed to send accept iq");
    });

    // launch ping thread
    self.pinger = std::thread([&self]() {
        while(true) {
            const auto iq = xmpp::elm::iq.clone()
                                .append_attrs({
                                    {"type", "get"},
                                })
                                .append_children({
                                    xmpp::elm::ping,
                                });
            self.conference->send_iq(iq, {});
            std::this_thread::sleep_for(std::chrono::seconds(10));
        }
    });

    return true;
}

struct XMPPNegotiatorCallbacks : public xmpp::NegotiatorCallbacks {
    ws::Connection* ws_conn;

    auto send_payload(std::string_view payload) -> void override {
        ws::send_str(ws_conn, payload);
    }
};

struct ConferenceCallbacks : public conference::ConferenceCallbacks {
    GstJitsiBin*    jitsibin;
    ws::Connection* ws_conn;
    JingleHandler*  jingle_handler;

    auto on_participant_joined_left(const conference::Participant& participant, const guint signal, const std::string_view debug_label) -> void {
        auto& self = *jitsibin->real_self;
        if(self.props.verbose) {
            print("participant ", debug_label, " id=", participant.participant_id, " nick=", participant.nick);
        }

        g_signal_emit(jitsibin, signal, 0, participant.participant_id.data(), participant.nick.data());
    }

    auto send_payload(std::string_view payload) -> void override {
        ws::send_str(ws_conn, payload);
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
};

auto null_to_ready(RealSelf& self) -> bool {
    assert_b(self.props.ensure_required_prop());

    const auto ws_path = std::string("xmpp-websocket?room=") + self.props.room_name;
    self.ws_conn       = ws::create_connection(self.props.server_address.data(), 443, ws_path.data(), self.props.secure);

    // gain jid from server
    {
        auto negotiate_done_event = Event();
        auto callbacks            = XMPPNegotiatorCallbacks();
        callbacks.ws_conn         = self.ws_conn;
        const auto negotiator     = xmpp::Negotiator::create(self.props.server_address, &callbacks);
        ws::add_receiver(self.ws_conn, [&negotiator, &negotiate_done_event](const std::span<std::byte> data) -> ws::ReceiverResult {
            const auto payload = std::string_view(std::bit_cast<char*>(data.data()), data.size());
            const auto done    = negotiator->feed_payload(payload);
            if(done) {
                negotiate_done_event.wakeup();
                return ws::ReceiverResult::Complete;
            } else {
                return ws::ReceiverResult::Handled;
            }
        });
        negotiator->start_negotiation();
        negotiate_done_event.wait();

        self.jid              = std::move(negotiator->jid);
        self.extenal_services = std::move(negotiator->external_services);
    }

    const auto jingle_handler = new JingleHandler(self.props.audio_codec_type,
                                                  self.props.video_codec_type,
                                                  self.jid,
                                                  self.extenal_services,
                                                  &self.session_initiate_jingle_arrived_event);
    self.jingle_handler.reset(jingle_handler);

    // join to conference
    const auto callbacks      = new ConferenceCallbacks();
    callbacks->jitsibin       = GST_JITSIBIN(self.bin);
    callbacks->ws_conn        = self.ws_conn;
    callbacks->jingle_handler = jingle_handler;
    self.conference_callbacks.reset(callbacks);
    self.conference = conference::Conference::create(
        conference::Config{
            .jid              = self.jid,
            .room             = self.props.room_name,
            .nick             = self.props.nick,
            .video_codec_type = self.props.video_codec_type,
            .audio_muted      = false,
            .video_muted      = false,
        },
        callbacks);
    ws::add_receiver(self.ws_conn, [&self](const std::span<std::byte> data) -> ws::ReceiverResult {
        // feed_payload always returns true in current implementation
        self.conference->feed_payload(std::string_view((char*)data.data(), data.size()));
        return ws::ReceiverResult::Handled;
    });
    self.conference->start_negotiation();

    /*
     * if there are no participants in the conference, jicofo does not send session-initiate jingle.
     * therefore, wait_for_jingle_and_setup_pipeline is a blocking function.
     */
    if(self.props.async_sink) {
        assert_b(setup_stub_pipeline(self));
        // wait for jingle in another thread, in order to avoid blocking entire pipeline.
        self.session_initiate_jingle_wait_thread = std::thread([&self]() {
            wait_for_jingle_and_setup_pipeline(self);
        });
    } else {
        wait_for_jingle_and_setup_pipeline(self);
    }

    return true;
}

auto ready_to_null(RealSelf& self) -> bool {
    ws::free_connection(self.ws_conn);
    // TODO: finish pinger
    return true;
}

auto change_state(GstElement* element, const GstStateChange transition) -> GstStateChangeReturn {
    const auto jitsibin = GST_JITSIBIN(element);
    auto&      self     = *jitsibin->real_self;

    auto ret = GST_ELEMENT_CLASS(parent_class)->change_state(element, transition);
    assert_v(ret != GST_STATE_CHANGE_FAILURE, GST_STATE_CHANGE_FAILURE);

    switch(transition) {
    case GST_STATE_CHANGE_NULL_TO_READY:
        assert_v(null_to_ready(self), GST_STATE_CHANGE_FAILURE);
        break;
    case GST_STATE_CHANGE_READY_TO_PAUSED:
        ret = GST_STATE_CHANGE_NO_PREROLL;
        break;
    case GST_STATE_CHANGE_PLAYING_TO_PAUSED:
        ret = GST_STATE_CHANGE_NO_PREROLL;
        break;
    case GST_STATE_CHANGE_READY_TO_NULL:
        assert_v(ready_to_null(self), GST_STATE_CHANGE_FAILURE);
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
    unwrap_pn_mut(jitsibin_audio_sink, gst_ghost_pad_new_no_target("audio_sink", GST_PAD_SINK));
    assert_n(gst_element_add_pad(GST_ELEMENT(self.bin), &jitsibin_audio_sink) == TRUE);
    self.audio_sink_elements.sink_pad = &jitsibin_audio_sink;
    // video sink
    unwrap_pn_mut(jitsibin_video_sink, gst_ghost_pad_new_no_target("video_sink", GST_PAD_SINK));
    assert_n(gst_element_add_pad(GST_ELEMENT(self.bin), &jitsibin_video_sink) == TRUE);
    self.video_sink_elements.sink_pad = &jitsibin_video_sink;

    return;
}

auto gst_jitsibin_finalize(GObject* object) -> void {
    const auto jitsibin = GST_JITSIBIN(object);
    delete jitsibin->real_self;
    G_OBJECT_CLASS(parent_class)->finalize(object);
}

auto gst_jitsibin_class_init(GstJitsiBinClass* klass) -> void {
    GST_DEBUG_CATEGORY_INIT(jitsibin_debug, "jitsibin", 0, "jitsibin");

    klass->participant_joined_signal = g_signal_new("participant-joined", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_FIRST, 0, NULL, NULL, NULL, G_TYPE_NONE, 2, G_TYPE_STRING, G_TYPE_STRING);
    klass->participant_left_signal   = g_signal_new("participant-left", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_FIRST, 0, NULL, NULL, NULL, G_TYPE_NONE, 2, G_TYPE_STRING, G_TYPE_STRING);

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

    const auto bin_class = (GstBinClass*)(klass);
}
