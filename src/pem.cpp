#include "jitsi/base64.hpp"

namespace pem {
auto encode(const std::string_view label, const std::span<const std::byte> bytes) -> std::string {
    auto r = std::string();

    r += "-----BEGIN ";
    r += label;
    r += "-----";
    r += '\n';

    auto b64 = base64::encode(bytes);
    auto pos = 0u;
    while(pos + 64 < b64.size()) {
        r += b64.substr(pos, 64);
        r += '\n';
        pos += 64;
    }
    r += b64.substr(pos);
    r += '\n';

    r += "-----END ";
    r += label;
    r += "-----";

    return r;
}
} // namespace pem
