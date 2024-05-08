#pragma once
#include <span>
#include <unordered_map>

#include "ice.hpp"
#include "jitsi/array-util.hpp"
#include "jitsi/jingle-handler.hpp"
#include "jitsi/xmpp/extdisco.hpp"
#include "jitsi/xmpp/jid.hpp"
#include "util/event.hpp"

enum class CodecType {
    Opus,
    H264,
    Vp8,
    Vp9,
};

inline const auto codec_type_str = make_str_table<CodecType>({
    {CodecType::Opus, "opus"},
    {CodecType::H264, "H264"},
    {CodecType::Vp8, "VP8"},
    {CodecType::Vp9, "VP9"},
});

struct Codec {
    using RTCPFeedBack = jingle::Jingle::Content::RTPDescription::PayloadType::RTCPFeedBack;

    CodecType type;
    int       tx_pt;
    int       rtx_pt = -1;

    std::vector<RTCPFeedBack> rtcp_fbs;
};

constexpr auto rtp_hdrext_ssrc_audio_level_uri = "urn:ietf:params:rtp-hdrext:ssrc-audio-level";
constexpr auto rtp_hdrext_transport_cc_uri     = "http://www.ietf.org/id/draft-holmer-rmcat-transport-wide-cc-extensions-01";

enum class SourceType {
    Audio,
    Video,
};

struct Source {
    uint32_t    ssrc;
    SourceType  type;
    std::string participant_id;
};

using SSRCMap = std::unordered_map<uint32_t, Source>;

struct JingleSession {
    jingle::Jingle       initiate_jingle;
    ice::Agent           ice_agent;
    ice::LocalCredential local_cred;
    std::string          fingerprint_str;
    std::string          dtls_cert_pem;
    std::string          dtls_priv_key_pem;
    std::vector<Codec>   codecs;
    SSRCMap              ssrc_map;
    uint32_t             audio_ssrc;
    uint32_t             video_ssrc;
    uint32_t             video_rtx_ssrc;
    int                  video_hdrext_transport_cc     = -1;
    int                  audio_hdrext_transport_cc     = -1;
    int                  audio_hdrext_ssrc_audio_level = -1;

    auto find_codec_by_type(CodecType type) const -> const Codec*;
    auto find_codec_by_tx_pt(int tx_pt) const -> const Codec*;
};

class GstJingleHandler : public JingleHandler {
  private:
    Event*                         sync;
    CodecType                      audio_codec_type;
    CodecType                      video_codec_type;
    xmpp::Jid                      jid;
    std::span<const xmpp::Service> external_services;
    JingleSession                  session;

  public:
    auto get_session() const -> const JingleSession&;
    auto build_accept_jingle() const -> std::optional<jingle::Jingle>;

    auto initiate(jingle::Jingle jingle) -> bool override;
    auto add_source(jingle::Jingle jingle) -> bool override;

    GstJingleHandler(CodecType                      audio_codec_type,
                     CodecType                      video_codec_type,
                     xmpp::Jid                      jid,
                     std::span<const xmpp::Service> external_services,
                     Event*                         sync);
};
