#include <span>
#include <thread>

#include <gst/rtp/gstrtpbasedepayload.h>
#include <gst/rtp/gstrtpdefs.h>
#include <gst/rtp/gstrtphdrext.h>

#include "auto-gst-object.hpp"
#include "autoptr.hpp"
#include "jitsi/unwrap.hpp"
#include "jitsi/util/charconv.hpp"
#include "jitsi/util/event.hpp"
#include "jitsi/util/misc.hpp"
#include "jitsi/xmpp/connection.hpp"
#include "jitsibin.hpp"

#define GST_CAT_DEFAULT jitsibin_debug

#define gst_jitsibin_parent_class parent_class
G_DEFINE_TYPE(GstJitsiBin, gst_jitsibin, GST_TYPE_BIN);

namespace {
GST_DEBUG_CATEGORY_STATIC(jitsibin_debug);

#define call_vfunc(self, func, ...) \
    GST_BIN_GET_CLASS(&self.bin)->func(&self.bin, __VA_ARGS__)

declare_autoptr(GstStructure, GstStructure, gst_structure_free);
declare_autoptr(GString, gchar, g_free);

auto codec_type_to_payloader_name(const CodecType type) -> const char* {
    switch(type) {
    case CodecType::Opus:
        return "rtpopuspay";
    case CodecType::H264:
        return "rtph264pay";
    case CodecType::Vp8:
        return "rtpvp8pay";
    case CodecType::Vp9:
        return "rtpvp9pay";
    default:
        return nullptr;
    }
}

auto codec_type_to_depayloader_name(const CodecType type) -> const char* {
    switch(type) {
    case CodecType::Opus:
        return "rtpopusdepay";
    case CodecType::H264:
        return "rtph264depay";
    case CodecType::Vp8:
        return "rtpvp8depay";
    case CodecType::Vp9:
        return "rtpvp9depay";
    default:
        return nullptr;
    }
}

auto set_prop(GObject* obj, const guint id, const GValue* value, GParamSpec* spec) -> void {
}

auto get_prop(GObject* obj, const guint id, GValue* value, GParamSpec* spec) -> void {
}

auto rtpbin_request_pt_map_handler(GstElement* const rtpbin, const guint session, const guint pt, const gpointer data) -> GstCaps* {
    PRINT("rtpbin request-pt-map session=", session, " pt=", pt);
    auto&       self           = *std::bit_cast<GstJitsiBin*>(data);
    const auto& jingle_session = self.jingle_handler->get_session();

    const auto caps = gst_caps_new_simple("application/x-rtp",
                                          "payload", G_TYPE_INT, pt,
                                          NULL);
    for(const auto& codec : jingle_session.codecs) {
        if(codec.tx_pt == int(pt)) {
            switch(codec.type) {
            case CodecType::Opus: {
                const auto encoding_name = "OPUS";
                gst_caps_set_simple(caps,
                                    "media", G_TYPE_STRING, "audio",
                                    "encoding-name", G_TYPE_STRING, encoding_name,
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
                const auto encoding_name = codec_type_str.find(codec.type);
                assert_p(encoding_name != nullptr, "codec_type_str bug");
                gst_caps_set_simple(caps,
                                    "media", G_TYPE_STRING, "video",
                                    "encoding-name", G_TYPE_STRING, encoding_name->second,
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
    PRINT("unknown payload type requested");
    return NULL;
}

auto rtpbin_new_jitterbuffer_handler(GstElement* const rtpbin, GstElement* const jitterbuffer, const guint session, const guint ssrc, gpointer const data) -> void {
    PRINT("rtpbin new-jitterbuffer session=", session, " ssrc=", ssrc);
    auto&       self           = *std::bit_cast<GstJitsiBin*>(data);
    const auto& jingle_session = self.jingle_handler->get_session();

    auto source = (const Source*)(nullptr);
    if(const auto i = jingle_session.ssrc_map.find(ssrc); i != jingle_session.ssrc_map.end()) {
        source = &i->second;
    }
    if(source == nullptr) {
        for(auto i = jingle_session.ssrc_map.begin(); i != jingle_session.ssrc_map.end(); i = std::next(i)) {
            PRINT("known ssrc: ", i->second.ssrc, " ", i->second.participant_id);
        }
    }
    assert_n(source != nullptr, "unknown ssrc");
    PRINT("jitterbuffer is for remote source ", source->participant_id);
    if(source->type != SourceType::Video) {
        return;
    }
    PRINT("enabling RTX");

    // TODO
    constexpr auto buffer_latency_milliseconds = 200;

    g_object_set(jitterbuffer,
                 "do-retransmission", TRUE,
                 "drop-on-latency", TRUE,
                 "latency", buffer_latency_milliseconds,
                 NULL);
}

auto aux_handler_create_pt_map(const std::span<const Codec> codecs) -> AutoGstStructure {
    auto pt_map = AutoGstStructure(gst_structure_new_empty("application/x-rtp-pt-map"));
    for(const auto& codec : codecs) {
        // FIXME: remove this check
        if(codec.type == CodecType::Opus) {
            continue;
        }
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
    PRINT("rtpbin request-aux-sender session=", session);
    auto&       self           = *std::bit_cast<GstJitsiBin*>(data);
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
    PRINT("rtpbin request-aux-receiver session=", session);
    auto&       self           = *std::bit_cast<GstJitsiBin*>(data);
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
    PRINT("(de)payloader extension request ext_id=", ext_id, " ext_uri=", ext_uri);

    auto ext = gst_rtp_header_extension_create_from_uri(ext_uri);
    assert_p(ext != NULL);
    gst_rtp_header_extension_set_id(ext, ext_id);
    return ext;
}

auto rtpbin_pad_added_handler(GstElement* const rtpbin, GstPad* const pad, gpointer const data) -> void {
    PRINT("rtpbin pad_added");
    auto&       self           = *std::bit_cast<GstJitsiBin*>(data);
    const auto& jingle_session = self.jingle_handler->get_session();

    const auto name_g = AutoGString(gst_object_get_name(GST_OBJECT(pad)));
    const auto name   = std::string_view(name_g.get());
    PRINT("pad name=", name);
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
    PRINT("pad added for remote source ", source->participant_id);

    // add depayloader
    unwrap_pn(codec, jingle_session.find_codec_by_tx_pt(pt), "cannot find depayloader for such payload type");
    const auto depay = AutoGstObject(gst_element_factory_make(codec_type_to_depayloader_name(codec.type), NULL));
    g_object_set(depay.get(),
                 "auto-header-extension", FALSE,
                 NULL);
    g_signal_connect(depay.get(), "request-extension", G_CALLBACK(pay_depay_request_extension_handler), &self);
    assert_n(call_vfunc(self, add_element, depay.get()) == TRUE);
    assert_n(gst_element_sync_state_with_parent(depay.get()));
    const auto depay_sink_pad = AutoGstObject(gst_element_get_static_pad(depay.get(), "sink"));
    assert_n(depay_sink_pad.get() != NULL);
    assert_n(gst_pad_link(pad, GST_PAD(depay_sink_pad.get())) == GST_PAD_LINK_OK);

    if(false) {
        // add fakesink
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
    const auto encoding_name = codec_type_str.find(codec.type);
    assert_n(encoding_name != nullptr, "codec_type_str bug");
    const auto ghost_pad_name = build_string(source->participant_id, "_", encoding_name->second, "_", ssrc);

    const auto depay_src_pad = AutoGstObject(gst_element_get_static_pad(depay.get(), "src"));
    assert_n(depay_src_pad.get() != NULL);

    const auto ghost_pad = AutoGstObject(gst_ghost_pad_new(ghost_pad_name.data(), depay_src_pad.get()));
    assert_n(ghost_pad.get() != NULL);

    assert_n(gst_element_add_pad(GST_ELEMENT(&self), ghost_pad.get()) == TRUE);

    return;
}

auto construct_sub_pipeline(GstJitsiBin& self, const CodecType audio_codec_type, const CodecType video_codec_type) -> bool {
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
    assert_b(dtlssrtpenc != NULL, "failed to create dtlssrtpdec");
    g_object_set(dtlssrtpdec,
                 "connection-id", dtls_conn_id.data(),
                 "pem", (jingle_session.dtls_cert_pem + "\n" + jingle_session.dtls_priv_key_pem).data(),
                 NULL);
    assert_b(call_vfunc(self, add_element, dtlssrtpdec) == TRUE);

    // audio payloader
    unwrap_pb(audio_codec, jingle_session.find_codec_by_type(audio_codec_type));
    const auto audio_pay_name = codec_type_to_payloader_name(audio_codec_type);
    const auto audio_pay      = gst_element_factory_make(audio_pay_name, NULL);
    assert_b(audio_pay != NULL, "failed to create audio payloader");
    g_object_set(audio_pay,
                 "pt", audio_codec.tx_pt,
                 "ssrc", jingle_session.audio_ssrc,
                 NULL);
    switch(audio_codec_type) {
    case CodecType::Opus:
        g_object_set(audio_pay,
                     "min-ptime", 10u * 1000 * 1000, // 10ms
                     NULL);
        break;
    default:
        PRINT("codec type bug");
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
    unwrap_pb(video_codec, jingle_session.find_codec_by_type(video_codec_type));
    const auto video_pay_name = codec_type_to_payloader_name(video_codec_type);
    const auto video_pay      = gst_element_factory_make(video_pay_name, NULL);
    assert_b(video_pay != NULL, "failed to create video payloader");
    g_object_set(video_pay,
                 "pt", video_codec.tx_pt,
                 "ssrc", jingle_session.video_ssrc,
                 NULL);
    switch(video_codec_type) {
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
        PRINT("codec type bug");
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

    // expose sink pad
    unwrap_pb_mut(video_pay_sink, gst_element_get_static_pad(video_pay, "sink"));
    unwrap_pb_mut(jitsibin_sink, gst_ghost_pad_new("sink", &video_pay_sink));
    assert_b(gst_element_add_pad(GST_ELEMENT(&self), &jitsibin_sink) == TRUE);
    self.sink = &jitsibin_sink;

    return true;
}
} // namespace

auto gst_jitsibin_init(GstJitsiBin* jitsibin) -> void {
    GST_LOG("init");

    auto& self = *jitsibin;

    constexpr auto audio_codec_type = CodecType::Opus;
    constexpr auto video_codec_type = CodecType::H264; // TODO

    constexpr auto host = "jitsi.local";
    constexpr auto room = "sink1";
    self.ws_conn        = ws::connect(host, (std::string("xmpp-websocket?room=") + room).data());

    auto ws_tx = [&self](const std::string_view str) {
        ws::send_str(self.ws_conn, str);
    };

    auto event  = Event();
    auto jid    = xmpp::Jid();
    auto ext_sv = std::vector<xmpp::Service>();

    // connect to server
    {
        const auto xmpp_conn = xmpp::create(host, ws_tx);
        ws::add_rx(self.ws_conn, [xmpp_conn, &event](const std::span<std::byte> data) -> ws::RxResult {
            const auto done = xmpp::resume_negotiation(xmpp_conn, std::string_view((char*)data.data(), data.size()));
            if(done) {
                event.wakeup();
                return ws::RxResult::Complete;
            }
            return ws::RxResult::Handled;
        });
        xmpp::start_negotiation(xmpp_conn);
        event.wait();

        auto res = xmpp::finish(xmpp_conn);
        jid      = std::move(res.jid);
        ext_sv   = std::move(res.external_services);
    }
    event.clear();
    const auto jingle_handler = new GstJingleHandler(audio_codec_type, video_codec_type, jid, ext_sv, &event);
    new(&self.jingle_handler) std::unique_ptr<GstJingleHandler>(jingle_handler);
    // join to conference
    new(&self.conference) std::unique_ptr<conference::Conference>(conference::Conference::create(room, jid));
    self.conference->jingle_handler        = jingle_handler;
    self.conference->send_payload          = ws_tx;
    self.conference->on_participant_joined = [](const conference::Participant& p) -> void {
        PRINT("partitipant joined ", p.participant_id, " ", p.nick);
    };
    self.conference->on_participant_left = [](const conference::Participant& p) -> void {
        PRINT("partitipant left ", p.participant_id, " ", p.nick);
    };
    ws::add_rx(self.ws_conn, [&self, &event](const std::span<std::byte> data) -> ws::RxResult {
        const auto done = self.conference->feed_payload(std::string_view((char*)data.data(), data.size()));
        if(done) {
            event.wakeup();
            return ws::RxResult::Complete;
        }
        return ws::RxResult::Handled;
    });
    self.conference->start_negotiation();
    // wait for jingle initiation
    event.wait();
    event.clear();
    PRINT("creating pipeline");
    // create pipeline based on the jingle information
    DYN_ASSERT(construct_sub_pipeline(self, audio_codec_type, video_codec_type));
    // accept jingle session
    unwrap_on_mut(accept, jingle_handler->build_accept_jingle());
    self.conference->send_jingle_accept(std::move(accept));

    static auto pinger = std::thread([jitsibin]() {
        auto count = 1;
        while(true) {
            const auto payload = build_string(R"(<iq xmlns='jabber:client' id=")", "ping_",
                                              count += 1,
                                              R"(" type="get"><ping xmlns='urn:xmpp:ping'/></iq>)");
            ws::send_str(jitsibin->ws_conn, payload);
            std::this_thread::sleep_for(std::chrono::seconds(5));
        }
    });

    return;
    while(true) {
        event.clear();
        event.wait();
    }
}

auto gst_jitsibin_finalize(GObject* object) -> void {
    auto& self = *GST_JITSIBIN(object);

    ws::free_connection(self.ws_conn);
    self.jingle_handler.reset();

    G_OBJECT_CLASS(parent_class)->finalize(object);
}

auto gst_jitsibin_class_init(GstJitsiBinClass* klass) -> void {
    GST_DEBUG_CATEGORY_INIT(jitsibin_debug, "jitsibin", 0, "jitsibin");

    parent_class = g_type_class_peek_parent(klass);

    const auto gobject_class    = (GObjectClass*)(klass);
    gobject_class->finalize     = gst_jitsibin_finalize;
    gobject_class->set_property = set_prop;
    gobject_class->get_property = get_prop;

    const auto element_class = (GstElementClass*)(klass);
    // gst_element_class_add_static_pad_template(element_class, &src_pad_template);
    // gst_element_class_add_static_pad_template(element_class, &sink_pad_template);
    gst_element_class_set_static_metadata(element_class,
                                          "Jitsi Meet Bin",
                                          "Filter/Network/RTP",
                                          "Jitsi Meet Bin",
                                          "mojyack <mojyack@gmail.com>");

    const auto bin_class = (GstBinClass*)(klass);
}
