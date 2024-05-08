#pragma once
#include <span>
#include <string>

namespace pem {
auto encode(std::string_view label, std::span<const std::byte> bytes) -> std::string;
}

