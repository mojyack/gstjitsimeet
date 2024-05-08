#include <string>

#include <netdb.h>

auto hostname_to_addr(const char* const hostname) -> std::string {
    auto r = std::string();

    const auto host = gethostbyname(hostname);
    if(host == NULL || host->h_length != 4 || host->h_addrtype != AF_INET) {
        return r;
    }
    for(auto i = 0; host->h_addr_list[i]; i += 1) {
        auto addr = (uint8_t*)host->h_addr_list[i];
        for(auto i = 0; i < 4; i += 1) {
            r += std::to_string(addr[i]);
            r += ".";
        }
        r.pop_back();
    }
    return r;
}
