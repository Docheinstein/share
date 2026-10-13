#include "addrutils.h"

#include <arpa/inet.h>
#include <bit>
#include <cstdio>
#include <cstdlib>

in_addr_t str_to_addr(const std::string& addr_str) {
    return inet_addr(addr_str.c_str());
}

in_port_t str_to_port(const std::string& addr_str) {
    return inet_addr(addr_str.c_str());
}

std::string addr_to_str(const in_addr addr) {
    char addr_str[INET_ADDRSTRLEN] {};
    if (!inet_ntop(AF_INET, &addr, addr_str, sizeof(addr_str))) {
        perror("inet_ntop");
        exit(1);
    }

    return addr_str;
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
