#ifndef ADDRUTILS_H
#define ADDRUTILS_H

#include <cstdint>
#include <netinet/in.h>
#include <string>

in_addr_t str_to_addr(const std::string& addr_str);
in_port_t str_to_port(const std::string& port_str);

std::string addr_to_str(in_addr addr);
std::string port_to_str(in_port_t port);

uint64_t htonll(uint64_t h);
uint64_t ntohll(uint64_t n);

#endif // ADDRUTILS_H
