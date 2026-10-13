#ifndef SOCKUTILS_H
#define SOCKUTILS_H

#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>

int socket_create_udp();
int socket_create_tcp();

sockaddr_in socket_create_addr(in_addr_t addr, in_port_t port);

void socket_set_option(int socket, int opt, int value);

void socket_connect(int socket, sockaddr_in socket_addr);

void socket_shutdown(int socket, int how);
void socket_shutdown_read(int socket);
void socket_shutdown_write(int socket, int how = SHUT_RDWR);
void socket_shutdown_read_write(int socket, int how = SHUT_RDWR);

void socket_bind(int socket, sockaddr_in socket_addr);
bool socket_try_bind(int socket, sockaddr_in socket_addr);

void socket_listen(int socket, int max_connections = 16);

int socket_accept(int socket, sockaddr_in* from_out = nullptr);

int socket_poll(pollfd* fds, nfds_t num_fds, int timeout = -1 /* none */);

size_t socket_send_from(int source_socket, const void* data, size_t size, const sockaddr_in* target_socket_addr);
size_t socket_send(int source_socket, const void* data, size_t size);

size_t socket_recv_from(int source_socket, void* data, size_t size, sockaddr_in* from_socket_addr);
size_t socket_recv(int source_socket, void* data, size_t size);
bool socket_recv_all(int source_socket, void* data, size_t size);

bool socket_wait_until(int socket, int timeout_ms);

#endif // SOCKUTILS_H
