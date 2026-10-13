#include "addrutils.h"

#include <bit>
#include <charconv>
#include <cstdio>
#include <cstdlib>

in_addr_t str_to_addr(const std::string& addr_str) {
    in_addr addr {};
    if (!inet_pton(AF_INET, addr_str.c_str(), &addr)) {
        perror("inet_pton");
        exit(1);
    }

    return addr.s_addr;
}

in_port_t str_to_port(const std::string& port_str) {
    uint16_t port {};
    const char* const end = port_str.data() + port_str.size();
    const auto [ptr, ec] = std::from_chars(port_str.data(), end, port);

    if (ec != std::errc {} || ptr != end || port == 0) {
        exit(1);
    }

    return htons(port);
}

std::string addr_to_str(const in_addr addr) {
    char addr_str[INET_ADDRSTRLEN] {};
    if (!inet_ntop(AF_INET, &addr, addr_str, sizeof(addr_str))) {
        perror("inet_ntop");
        exit(1);
    }

    return addr_str;
}

std::string addr_to_str(const in_addr_t addr) {
    in_addr in {};
    in.s_addr = addr;
    return addr_to_str(in);
}

std::string port_to_str(const in_port_t port) {
    return std::to_string(ntohs(port));
}

uint64_t htonll(const uint64_t h) {
    if constexpr (std::endian::native == std::endian::little) {
        return std::byteswap(h);
    }

    return h;
}

uint64_t ntohll(const uint64_t n) {
    return htonll(n);
}
