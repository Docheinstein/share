#include "sockutils.h"

#include <cstdio>
#include <cstdlib>

int socket_create_udp() {
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) {
        perror("socket");
        exit(1);
    }

    return s;
}

int socket_create_tcp() {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) {
        perror("socket");
        exit(1);
    }

    return s;
}

sockaddr_in socket_create_addr(in_addr_t addr, in_port_t port) {
    sockaddr_in socket_addr {};
    socket_addr.sin_family = AF_INET;
    socket_addr.sin_addr.s_addr = addr;
    socket_addr.sin_port = port;
    return socket_addr;
}

void socket_set_option(int socket, int opt, int value) {
    if (setsockopt(socket, SOL_SOCKET, opt, &value, sizeof value) < 0) {
        perror("setsockopt");
        exit(1);
    }
}

void socket_connect(int socket, sockaddr_in socket_addr) {
    if (connect(socket, reinterpret_cast<sockaddr*>(&socket_addr), sizeof(socket_addr)) < 0) {
        perror("connect");
        exit(1);
    }
}

void socket_shutdown(int socket, int how) {
    if (shutdown(socket, how) < 0) {
        perror("shutdown");
        exit(1);
    }
}

void socket_shutdown_read(int socket) {
    socket_shutdown(socket, SHUT_RD);
}

void socket_shutdown_write(int socket, int how) {
    socket_shutdown(socket, SHUT_WR);
}

void socket_shutdown_read_write(int socket, int how) {
    socket_shutdown(socket, SHUT_RDWR);
}

void socket_bind(int socket, sockaddr_in socket_addr) {
    if (bind(socket, reinterpret_cast<sockaddr*>(&socket_addr), sizeof(socket_addr)) < 0) {
        perror("bind");
        exit(1);
    }
}

bool socket_try_bind(int socket, sockaddr_in socket_addr) {
    return bind(socket, reinterpret_cast<sockaddr*>(&socket_addr), sizeof(socket_addr)) == 0;
}

void socket_listen(int socket, int max_connections) {
    if (listen(socket, max_connections) < 0) {
        perror("listen");
        exit(1);
    }
}

int socket_accept(int socket, sockaddr_in* from_out) {
    sockaddr_in from {};
    socklen_t from_len = sizeof(from);

    int in_tcp_sock = accept(socket, reinterpret_cast<sockaddr*>(&from), &from_len);

    if (in_tcp_sock < 0) {
        perror("accept");
        exit(1);
    }

    if (from_out) {
        *from_out = from;
    }

    return in_tcp_sock;
}

int socket_poll(pollfd* fds, nfds_t num_fds, int timeout) {
    int num_events = poll(fds, num_fds, timeout);
    if (num_events < 0) {
        perror("poll");
        exit(1);
    }

    return num_events;
}

size_t socket_send_from(int source_socket, const void* data, size_t size, const sockaddr_in* target_socket_addr) {
    ssize_t ret = sendto(source_socket, data, size, 0, reinterpret_cast<const sockaddr*>(target_socket_addr),
                         sizeof(sockaddr_in));

    if (ret < 0) {
        perror("sendto");
        exit(1);
    }

    return ret;
}

size_t socket_send(int source_socket, const void* data, size_t size) {
    ssize_t ret = send(source_socket, data, size, 0);

    if (ret < 0) {
        perror("send");
        exit(1);
    }

    return ret;
}

size_t socket_recv_from(int source_socket, void* data, size_t size, sockaddr_in* from_socket_addr) {
    socklen_t from_socket_addr_size = sizeof(sockaddr_in);
    const ssize_t ret =
        recvfrom(source_socket, data, size, 0, reinterpret_cast<sockaddr*>(from_socket_addr), &from_socket_addr_size);
    if (ret < 0) {
        perror("recvfrom");
        exit(1);
    }

    return ret;
}

size_t socket_recv(int source_socket, void* data, size_t size) {
    ssize_t ret = recv(source_socket, data, size, 0);
    if (ret < 0) {
        perror("recv");
        exit(1);
    }

    return ret;
}

bool socket_recv_all(int source_socket, void* data, size_t size) {
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

bool socket_wait_until(int socket, int timeout_ms) {
    pollfd fds {socket, POLLIN, 0};
    return poll(&fds, 1, timeout_ms) > 0;
}
