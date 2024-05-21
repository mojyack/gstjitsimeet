#pragma once
#include "../macros/unwrap.hpp"
#include "../util/assert.hpp"
#include "../util/charconv.hpp"

struct ParsedJitsbinPad {
    std::string_view participant_id;
    std::string_view codec;
    uint32_t         ssrc;
};

inline auto parse_jitsibin_pad_name(const std::string_view name) -> std::optional<ParsedJitsbinPad> {
    const auto i = name.rfind("_");
    assert_o(i != name.npos, "malformed pad name");
    const auto ssrc_str = name.substr(i + 1);
    unwrap_oo(ssrc, from_chars<uint32_t>(ssrc_str), "non numeric ssrc");
    const auto j = name.rfind("_", i - 1);
    assert_o(j != name.npos, "malformed pad name");
    const auto codec = name.substr(j + 1, i - j - 1);
    const auto id    = name.substr(0, j);
    return ParsedJitsbinPad{
        .participant_id = id,
        .codec          = codec,
        .ssrc           = ssrc,
    };
}
