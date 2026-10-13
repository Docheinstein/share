#ifndef SOCKUTILS_H
#define SOCKUTILS_H

#include <cstddef>
#include <poll.h>

#include "net.h"

socket_t socket_create_udp();
socket_t socket_create_tcp();

sockaddr_in socket_create_addr(in_addr_t addr, in_port_t port);

void socket_set_option(socket_t socket, int opt, int value);
void socket_set_reuse_addr_exclusive(socket_t socket);

void socket_connect(socket_t socket, sockaddr_in socket_addr);

void socket_shutdown(socket_t socket, int how);
void socket_shutdown_read(socket_t socket);
void socket_shutdown_write(socket_t socket, int how = SHUT_RDWR);
void socket_shutdown_read_write(socket_t socket, int how = SHUT_RDWR);

void socket_bind(socket_t socket, sockaddr_in socket_addr);
bool socket_try_bind(socket_t socket, sockaddr_in socket_addr);

void socket_listen(socket_t socket, int max_connections = 16);

socket_t socket_accept(socket_t socket, sockaddr_in* from_out = nullptr);

int socket_poll(pollfd* fds, nfds_t num_fds, int timeout = -1 /* none */);

size_t socket_send_from(socket_t source_socket, const void* data, size_t size, const sockaddr_in* target_socket_addr);
size_t socket_send(socket_t source_socket, const void* data, size_t size);

size_t socket_recv_from(socket_t source_socket, void* data, size_t size, sockaddr_in* from_socket_addr);
size_t socket_recv(socket_t source_socket, void* data, size_t size);
bool socket_recv_all(socket_t source_socket, void* data, size_t size);

bool socket_wait_until(socket_t socket, int timeout_ms);

void socket_close(socket_t socket);

#endif // SOCKUTILS_H
