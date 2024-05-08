#include <iomanip>

#include "cert.hpp"
#include "jingle.hpp"
#include "jitsi/assert.hpp"
#include "jitsi/jingle/jingle.hpp"
#include "jitsi/sha.hpp"
#include "jitsi/unwrap.hpp"
#include "pem.hpp"
#include "util/charconv.hpp"
#include "util/error.hpp"
#include "util/result.hpp"

namespace {
template <class T>
auto replace_default(T& num, const T val) -> void {
    num = num == -1 ? val : num;
}

const auto source_type_str = make_str_table<SourceType>({
    {SourceType::Audio, "audio"},
    {SourceType::Video, "video"},
});

struct DescriptionParseResult {
    std::vector<Codec> codecs;

    int video_hdrext_transport_cc     = -1;
    int audio_hdrext_transport_cc     = -1;
    int audio_hdrext_ssrc_audio_level = -1;
};

auto parse_rtp_description(const jingle::Jingle::Content::RTPDescription& desc, SSRCMap& ssrc_map) -> Result<DescriptionParseResult, StringError> {
    auto source_type = SourceType();
    if(const auto e = source_type_str.find(desc.media); e != nullptr) {
        source_type = e->first;
    } else {
        return StringError("unknown media");
    }

    auto r = DescriptionParseResult{};

    // parse codecs
    for(const auto& pt : desc.payload_types) {
        if(pt.name == "rtx") {
            continue;
        }
        if(const auto e = codec_type_str.find(pt.name); e != nullptr) {
            auto codec = Codec{
                .type     = e->first,
                .tx_pt    = pt.id,
                .rtx_pt   = -1,
                .rtcp_fbs = pt.rtcp_fbs,
            };
            r.codecs.push_back(codec);
        } else {
            PRINT("unknown codec ", pt.name);
        }
    }
    // parse retransmission payload types
    for(const auto& pt : desc.payload_types) {
        if(pt.name != "rtx") {
            continue;
        }
        for(const auto& p : pt.parameters) {
            if(p.name != "apt") {
                continue;
            }
            const auto apt = from_chars<int>(p.value);
            if(!apt) {
                PRINT("invalid apt ", p.value);
                continue;
            }
            for(auto& codec : r.codecs) {
                if(codec.tx_pt == *apt) {
                    codec.rtx_pt = pt.id;
                    break;
                }
            }
            break;
        }
    }
    // parse extensions
    for(const auto& ext : desc.rtp_header_exts) {
        if(ext.uri == rtp_hdrext_ssrc_audio_level_uri) {
            r.audio_hdrext_ssrc_audio_level = ext.id;
        } else if(ext.uri == rtp_hdrext_transport_cc_uri) {
            switch(source_type) {
            case SourceType::Audio:
                r.audio_hdrext_transport_cc = ext.id;
                break;
            case SourceType::Video:
                r.video_hdrext_transport_cc = ext.id;
                break;
            }
        } else {
            PRINT("unsupported rtp header extension ", ext.uri);
        }
    }
    // parse ssrc
    for(const auto& source : desc.sources) {
        ssrc_map[source.ssrc] = Source{
            .ssrc           = source.ssrc,
            .type           = source_type,
            .participant_id = source.owner,
        };
    }
    return r;
}

auto digest_str(const std::span<const std::byte> digest) -> std::string {
    auto ss = std::stringstream();
    ss << std::hex;
    ss << std::uppercase;
    for(const auto b : digest) {
        ss << std::setw(2) << std::setfill('0') << static_cast<int>(b) << ":";
    }
    auto r = ss.str();
    r.pop_back();
    return r;
}
} // namespace

auto JingleSession::find_codec_by_type(const CodecType type) const -> const Codec* {
    for(const auto& codec : codecs) {
        if(codec.type == type) {
            return &codec;
        }
    }
    return nullptr;
}

auto JingleSession::find_codec_by_tx_pt(const int tx_pt) const -> const Codec* {
    for(const auto& codec : codecs) {
        if(codec.tx_pt == tx_pt) {
            return &codec;
        }
    }
    return nullptr;
}

auto GstJingleHandler::get_session() const -> const JingleSession& {
    return session;
}

auto GstJingleHandler::build_accept_jingle() const -> std::optional<jingle::Jingle> {
    auto& jingle = session.initiate_jingle;

    auto accept = jingle::Jingle{
        .action    = jingle::Jingle::Action::SessionAccept,
        .sid       = jingle.sid,
        .initiator = jingle.initiator,
        .responder = jid.as_full(),
    };
    for(auto i = 0; i < 2; i += 1) {
        const auto is_audio   = i == 0;
        const auto codec_type = is_audio ? audio_codec_type : video_codec_type;
        const auto main_ssrc  = is_audio ? session.audio_ssrc : session.video_ssrc;

        // build rtp description
        auto rtp_desc = jingle::Jingle::Content::RTPDescription{
            .media       = is_audio ? "audio" : "video",
            .ssrc        = main_ssrc,
            .support_mux = true,
        };
        // append payload type
        unwrap_po(codec, session.find_codec_by_type(codec_type));
        rtp_desc.payload_types.push_back(jingle::Jingle::Content::RTPDescription::PayloadType{
            .id        = codec.tx_pt,
            .clockrate = is_audio ? 48000 : 90000,
            .channels  = is_audio ? 2 : -1,
            .name      = codec_type_str.find(codec_type)->second,
            .rtcp_fbs  = codec.rtcp_fbs,
        });
        if(codec.rtx_pt != -1) {
            auto rtx_pt = jingle::Jingle::Content::RTPDescription::PayloadType{
                .id         = codec.rtx_pt,
                .clockrate  = is_audio ? 48000 : 90000,
                .channels   = is_audio ? 2 : -1,
                .name       = "rtx",
                .parameters = {{"apt", std::to_string(codec.tx_pt)}},
            };
            for(const auto& fb : codec.rtcp_fbs) {
                if(fb.type != "transport-cc") {
                    rtx_pt.rtcp_fbs.push_back(fb);
                }
            }
            rtp_desc.payload_types.push_back(std::move(rtx_pt));
        }
        // append source
        rtp_desc.sources.push_back(jingle::Jingle::Content::RTPDescription::Source{.ssrc = main_ssrc});
        if(!is_audio) {
            rtp_desc.sources.push_back(jingle::Jingle::Content::RTPDescription::Source{.ssrc = session.video_rtx_ssrc});
        }
        static auto stream_id_serial = std::atomic_int(0);
        const auto  stream_id        = stream_id_serial.fetch_add(1);
        const auto  label            = build_string("stream_label_", stream_id);
        const auto  mslabel          = build_string("multi_stream_label_", stream_id);
        const auto  msid             = mslabel + " " + label;
        const auto  cname            = build_string("cname_", stream_id);
        for(auto& src : rtp_desc.sources) {
            src.parameters.push_back({"cname", cname});
            src.parameters.push_back({"msid", msid});
        }
        // append hdrext
        if(is_audio) {
            rtp_desc.rtp_header_exts.push_back(
                jingle::Jingle::Content::RTPDescription::RTPHeaderExt{
                    .id  = session.audio_hdrext_ssrc_audio_level,
                    .uri = rtp_hdrext_ssrc_audio_level_uri,
                });
            rtp_desc.rtp_header_exts.push_back(
                jingle::Jingle::Content::RTPDescription::RTPHeaderExt{
                    .id  = session.audio_hdrext_transport_cc,
                    .uri = rtp_hdrext_transport_cc_uri,
                });
        } else {
            rtp_desc.rtp_header_exts.push_back(
                jingle::Jingle::Content::RTPDescription::RTPHeaderExt{
                    .id  = session.video_hdrext_transport_cc,
                    .uri = rtp_hdrext_transport_cc_uri,
                });
        }
        // append ssrc-group
        if(!is_audio) {
            rtp_desc.ssrc_groups.push_back(jingle::Jingle::Content::RTPDescription::SSRCGroup{
                .semantics = jingle::Jingle::Content::RTPDescription::SSRCGroup::Semantics::Fid,
                .ssrcs     = {session.video_ssrc, session.video_rtx_ssrc},
            });
        }
        // rtp description done

        // build transport
        auto transport = jingle::Jingle::Content::IceUdpTransport{
            .pwd   = session.local_cred.pwd.get(),
            .ufrag = session.local_cred.ufrag.get(),
        };
        // add candidates
        const auto local_candidates = ice::get_local_candidates(session.ice_agent);
        for(const auto lc : local_candidates.candidates) {
            unwrap_oo(type, ice::candidate_type_from_nice(lc->type));
            const auto addr = ice::sockaddr_to_str(lc->addr);
            assert_o(!addr.empty());
            const auto  port                = ice::sockaddr_to_port(lc->addr);
            static auto candidate_id_serial = std::atomic_int(0);
            transport.candidates.push_back(jingle::Jingle::Content::IceUdpTransport::Candidate{
                .component  = uint8_t(lc->component_id),
                .generation = 0,
                .port       = port,
                .priority   = lc->priority,
                .type       = type,
                .foundation = lc->foundation,
                .id         = build_string("candidate_", candidate_id_serial.fetch_add(1)),
                .ip_addr    = addr,
            });
        }
        // add fingerprint
        transport.fingerprints.push_back(jingle::Jingle::Content::IceUdpTransport::FingerPrint{
            .hash      = session.fingerprint_str,
            .hash_type = "sha-256",
            .setup     = "active",
            .required  = false,
        });
        // transport done

        accept.contents.push_back(
            jingle::Jingle::Content{
                .name              = is_audio ? "audio" : "video",
                .senders           = jingle::Jingle::Content::Senders::Both,
                .is_from_initiator = false,
                .descriptions      = {rtp_desc},
                .transports        = {transport},
            });
    }

    accept.group.reset(new jingle::Jingle::Group{.semantics = jingle::Jingle::Group::Semantics::Bundle,
                                                 .contents  = {"audio", "video"}});

    return accept;
}

auto GstJingleHandler::on_initiate(jingle::Jingle jingle) -> bool {
    auto codecs                        = std::vector<Codec>();
    auto ssrc_map                      = SSRCMap();
    auto video_hdrext_transport_cc     = -1;
    auto audio_hdrext_transport_cc     = -1;
    auto audio_hdrext_ssrc_audio_level = -1;
    auto transport                     = (const jingle::Jingle::Content::IceUdpTransport*)(nullptr);
    for(const auto& c : jingle.contents) {
        for(const auto& d : c.descriptions) {
            unwrap_re(desc, parse_rtp_description(d, ssrc_map));
            codecs.insert(codecs.end(), desc.codecs.begin(), desc.codecs.end());
            replace_default(video_hdrext_transport_cc, desc.video_hdrext_transport_cc);
            replace_default(audio_hdrext_transport_cc, desc.audio_hdrext_transport_cc);
            replace_default(audio_hdrext_ssrc_audio_level, desc.audio_hdrext_ssrc_audio_level);
        }
        if(!c.transports.empty()) {
            transport = &c.transports[0];
        }
    }

    const auto cert = cert::AutoCert(cert::cert_new());
    assert_b(cert);
    const auto cert_der = cert::serialize_cert_der(cert.get());
    assert_b(cert_der);
    const auto priv_key_der = cert::serialize_private_key_pkcs8_der(cert.get());
    assert_b(priv_key_der);
    const auto fingerprint     = sha::calc_sha256(*cert_der);
    auto       fingerprint_str = digest_str(fingerprint);
    auto       cert_pem        = pem::encode("CERTIFICATE", *cert_der);
    auto       priv_key_pem    = pem::encode("PRIVATE KEY", *priv_key_der);

    // DEBUG
    printf("%s\n", fingerprint_str.data());
    printf("%s\n", cert_pem.data());
    printf("%s\n", priv_key_pem.data());
    // TODO
    const auto audio_ssrc     = uint32_t(3111629862);
    const auto video_ssrc     = uint32_t(2087854985);
    const auto video_rtx_ssrc = uint32_t(438931176);

    unwrap_ob_mut(ice_agent, ice::setup(external_services, transport));
    unwrap_ob_mut(local_cred, ice::get_local_credentials(ice_agent));

    session = JingleSession{
        .initiate_jingle               = std::move(jingle),
        .ice_agent                     = std::move(ice_agent),
        .local_cred                    = std::move(local_cred),
        .fingerprint_str               = std::move(fingerprint_str),
        .dtls_cert_pem                 = std::move(cert_pem),
        .dtls_priv_key_pem             = std::move(priv_key_pem),
        .codecs                        = std::move(codecs),
        .ssrc_map                      = std::move(ssrc_map),
        .audio_ssrc                    = audio_ssrc,
        .video_ssrc                    = video_ssrc,
        .video_rtx_ssrc                = video_rtx_ssrc,
        .video_hdrext_transport_cc     = video_hdrext_transport_cc,
        .audio_hdrext_transport_cc     = audio_hdrext_transport_cc,
        .audio_hdrext_ssrc_audio_level = audio_hdrext_ssrc_audio_level,
    };

    // session initiation half-done
    // wakeup mainthread to create pipeline
    sync->wakeup();

    return true;
}

auto GstJingleHandler::on_add_source(jingle::Jingle jingle) -> bool {
    for(const auto& c : jingle.contents) {
        for(const auto& desc : c.descriptions) {
            auto type = SourceType();
            if(desc.media == "audio") {
                type = SourceType::Audio;
            } else if(desc.media == "video") {
                type = SourceType::Video;
            } else {
                PRINT("unknown media ", desc.media);
                continue;
            }
            for(const auto& src : desc.sources) {
                session.ssrc_map.insert(std::make_pair(src.ssrc, Source{
                                                                     .ssrc           = src.ssrc,
                                                                     .type           = type,
                                                                     .participant_id = src.owner,
                                                                 }));
            }
        }
    }
    return true;
}

GstJingleHandler::GstJingleHandler(const CodecType                audio_codec_type,
                                   const CodecType                video_codec_type,
                                   xmpp::Jid                      jid,
                                   std::span<const xmpp::Service> external_services,
                                   Event* const                   sync)
    : sync(sync),
      audio_codec_type(audio_codec_type),
      video_codec_type(video_codec_type),
      jid(std::move(jid)),
      external_services(external_services) {
}
