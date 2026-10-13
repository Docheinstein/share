#ifndef NET_H
#define NET_H

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>

using socket_t = SOCKET;
using in_addr_t = ULONG;
using in_port_t = USHORT;
using nfds_t = ULONG;

constexpr socket_t INVALID_SOCKET_HANDLE = INVALID_SOCKET;

#define SHUT_RD SD_RECEIVE
#define SHUT_WR SD_SEND
#define SHUT_RDWR SD_BOTH
#else
#include <arpa/inet.h>

using socket_t = int;

constexpr socket_t INVALID_SOCKET_HANDLE = -1;
#endif

#endif // NET_H
