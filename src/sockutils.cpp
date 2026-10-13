#include "sockutils.h"

#include <cstdio>
#include <cstdlib>

#ifndef _WIN32
#include <unistd.h>
#endif

namespace {
#ifdef _WIN32
// Initializes Winsock for the lifetime of the program.
struct WinsockInit {
    WinsockInit() {
        WSADATA wsa_data;
        if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
            fprintf(stderr, "WSAStartup: failed to initialize Winsock\n");
            exit(1);
        }
    }
    ~WinsockInit() {
        WSACleanup();
    }
} winsock_init;
#endif

void socket_perror(const char* what) {
#ifdef _WIN32
    fprintf(stderr, "%s: error %d\n", what, WSAGetLastError());
#else
    perror(what);
#endif
}

int poll_sockets(pollfd* fds, nfds_t num_fds, int timeout) {
#ifdef _WIN32
    return WSAPoll(fds, num_fds, timeout);
#else
    return poll(fds, num_fds, timeout);
#endif
}

// Windows reports a datagram larger than the buffer as an error instead of truncating it.
bool is_truncated_datagram_error() {
#ifdef _WIN32
    return WSAGetLastError() == WSAEMSGSIZE;
#else
    return false;
#endif
}

// Windows reports an ICMP "port unreachable" (e.g. a sender that already went away)
// as an error on the next recvfrom(): ignore those, as POSIX does.
void ignore_udp_connection_reset([[maybe_unused]] socket_t socket) {
#ifdef _WIN32
    constexpr DWORD SIO_UDP_CONNRESET_ = _WSAIOW(IOC_VENDOR, 12);
    BOOL report = FALSE;
    DWORD bytes_returned = 0;
    WSAIoctl(socket, SIO_UDP_CONNRESET_, &report, sizeof report, nullptr, 0, &bytes_returned, nullptr, nullptr);
#endif
}
} // namespace

socket_t socket_create_udp() {
    socket_t s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s == INVALID_SOCKET_HANDLE) {
        socket_perror("socket");
        exit(1);
    }

    ignore_udp_connection_reset(s);

    return s;
}

socket_t socket_create_tcp() {
    socket_t s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == INVALID_SOCKET_HANDLE) {
        socket_perror("socket");
        exit(1);
    }

    return s;
}

sockaddr_in socket_create_sockaddr(const in_addr addr, const in_port_t port) {
    sockaddr_in socket_addr {};
    socket_addr.sin_family = AF_INET;
    socket_addr.sin_addr = addr;
    socket_addr.sin_port = port;
    return socket_addr;
}

in_addr socket_create_addr(const in_addr_t a) {
    in_addr addr {};
    addr.s_addr = a;
    return addr;
}

void socket_set_option(const socket_t socket, const int opt, const int value) {
    if (setsockopt(socket, SOL_SOCKET, opt, reinterpret_cast<const char*>(&value), sizeof(value)) < 0) {
        socket_perror("setsockopt");
        exit(1);
    }
}

void socket_set_reuse_addr_exclusive(const socket_t socket) {
    // Allow a quick restart on the same port, but never bind it twice.
#ifdef _WIN32
    // On Windows SO_REUSEADDR would let two sockets bind the same port,
    // while a closed port can be bound again right away anyway.
    socket_set_option(socket, SO_EXCLUSIVEADDRUSE, 1);
#else
    socket_set_option(socket, SO_REUSEADDR, 1);
#endif
}

void socket_connect(const socket_t socket, sockaddr_in socket_addr) {
    if (connect(socket, reinterpret_cast<sockaddr*>(&socket_addr), sizeof(socket_addr)) < 0) {
        socket_perror("connect");
        exit(1);
    }
}

void socket_shutdown(const socket_t socket, const int how) {
    if (shutdown(socket, how) < 0) {
        socket_perror("shutdown");
        exit(1);
    }
}

void socket_shutdown_read(const socket_t socket) {
    socket_shutdown(socket, SHUT_RD);
}

void socket_shutdown_write(const socket_t socket, const int how) {
    socket_shutdown(socket, SHUT_WR);
}

void socket_shutdown_read_write(const socket_t socket, const int how) {
    socket_shutdown(socket, SHUT_RDWR);
}

void socket_bind(const socket_t socket, sockaddr_in socket_addr) {
    if (bind(socket, reinterpret_cast<sockaddr*>(&socket_addr), sizeof(socket_addr)) < 0) {
        socket_perror("bind");
        exit(1);
    }
}

bool socket_try_bind(const socket_t socket, sockaddr_in socket_addr) {
    return bind(socket, reinterpret_cast<sockaddr*>(&socket_addr), sizeof(socket_addr)) == 0;
}

void socket_listen(const socket_t socket, const int max_connections) {
    if (listen(socket, max_connections) < 0) {
        socket_perror("listen");
        exit(1);
    }
}

socket_t socket_accept(const socket_t socket, sockaddr_in* from_out) {
    sockaddr_in from {};
    socklen_t from_len = sizeof(from);

    socket_t in_tcp_sock = accept(socket, reinterpret_cast<sockaddr*>(&from), &from_len);

    if (in_tcp_sock == INVALID_SOCKET_HANDLE) {
        socket_perror("accept");
        exit(1);
    }

    if (from_out) {
        *from_out = from;
    }

    return in_tcp_sock;
}

int socket_poll(pollfd* fds, const nfds_t num_fds, const int timeout) {
    int num_events = poll_sockets(fds, num_fds, timeout);
    if (num_events < 0) {
        socket_perror("poll");
        exit(1);
    }

    return num_events;
}

size_t socket_send_from(const socket_t source_socket, const void* const data, const size_t size,
                        const sockaddr_in* const target_socket_addr) {
    const auto ret = sendto(source_socket, static_cast<const char*>(data), size, 0,
                            reinterpret_cast<const sockaddr*>(target_socket_addr), sizeof(sockaddr_in));

    if (ret < 0) {
        socket_perror("sendto");
        exit(1);
    }

    return ret;
}

size_t socket_send(const socket_t source_socket, const void* const data, const size_t size) {
    const auto ret = send(source_socket, static_cast<const char*>(data), size, 0);

    if (ret < 0) {
        socket_perror("send");
        exit(1);
    }

    return ret;
}

size_t socket_recv_from(const socket_t source_socket, void* const data, const size_t size,
                        sockaddr_in* from_socket_addr) {
    socklen_t from_socket_addr_size = sizeof(sockaddr_in);
    const auto ret = recvfrom(source_socket, static_cast<char*>(data), size, 0,
                              reinterpret_cast<sockaddr*>(from_socket_addr), &from_socket_addr_size);
    if (ret < 0) {
        if (is_truncated_datagram_error()) {
            return size;
        }

        socket_perror("recvfrom");
        exit(1);
    }

    return ret;
}

size_t socket_recv(const socket_t source_socket, void* const data, const size_t size) {
    const auto ret = recv(source_socket, static_cast<char*>(data), size, 0);
    if (ret < 0) {
        socket_perror("recv");
        exit(1);
    }

    return ret;
}

bool socket_recv_all(const socket_t source_socket, void* const data, const size_t size) {
    size_t read = 0;
    while (read < size) {
        size_t read_result = socket_recv(source_socket, static_cast<char*>(data) + read, size - read);
        if (read_result == 0) {
            return false;
        }
        read += read_result;
    }
    return true;
}

bool socket_wait_until(const socket_t socket, const int timeout_ms) {
    pollfd fds {socket, POLLIN, 0};
    return poll_sockets(&fds, 1, timeout_ms) > 0;
}

void socket_close(const socket_t socket) {
#ifdef _WIN32
    closesocket(socket);
#else
    close(socket);
#endif
}
