#pragma once
#include <optional>
#include <vector>

namespace cert {
struct Cert;

auto cert_new() -> Cert*;
auto cert_delete(const Cert* cert) -> void;
auto serialize_cert_der(const Cert* cert) -> std::optional<std::vector<std::byte>>;
auto serialize_private_key_der(const Cert* cert) -> std::optional<std::vector<std::byte>>;
auto serialize_private_key_pkcs8_der(const Cert* cert) -> std::optional<std::vector<std::byte>>;

struct CertDeleter {
    auto operator()(const Cert* const cert) -> void {
        cert_delete(cert);
    }
};
using AutoCert = std::unique_ptr<Cert, CertDeleter>;
} // namespace cert

