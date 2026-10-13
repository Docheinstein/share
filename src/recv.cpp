#include "recv.h"

#include <algorithm>
#include <arpa/inet.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

#include "addrutils.h"
#include "constants.h"
#include "pathutils.h"
#include "progress.h"
#include "sockutils.h"

namespace {
bool is_valid_discovery_request(const void* const data, const size_t size) {
    return size == sizeof(DISCOVERY_REQUEST_DATA) &&
           memcmp(data, DISCOVERY_REQUEST_DATA, sizeof(DISCOVERY_REQUEST_DATA)) == 0;
}
} // namespace

void command_recv(int argc, char** argv) {
    // share recv [<name>]
    // If <name> is not given, the name of the current directory is used as share name.
    const std::string share_name = argc ? argv[0] : std::filesystem::current_path().filename().string();
    if (share_name.size() > SHARE_NAME_MAX_LENGTH) {
        fprintf(stderr, "Share name is too long: %zu > %u\n", share_name.size(), SHARE_NAME_MAX_LENGTH);
        return;
    }

    // Create UDP socket (for discovery response).
    const int udp_socket = socket_create_udp();
    socket_set_option(udp_socket, SO_REUSEADDR, 1);

    const sockaddr_in udp_socket_address = socket_create_addr(INADDR_ANY, htons(SHARE_UDP_PORT));
    socket_bind(udp_socket, udp_socket_address);

    // Create TCP socket.
    const int tcp_socket = socket_create_tcp();
    socket_set_option(tcp_socket, SO_REUSEADDR, 1);

    // Pick the first available port.
    uint16_t tcp_port = SHARE_TCP_MIN_PORT - 1;
    bool tcp_socket_bound = false;
    do {
        tcp_socket_bound = socket_try_bind(tcp_socket, socket_create_addr(INADDR_ANY, htons(++tcp_port)));
    } while (tcp_port <= SHARE_TCP_MAX_PORT && !tcp_socket_bound);

    socket_listen(tcp_socket);

    static constexpr uint8_t POLL_UDP_SOCKET_INDEX = 0;
    static constexpr uint8_t POLL_TCP_SOCKET_INDEX = 1;
    static constexpr uint8_t POLL_NUM_SOCEKTS = 2;

    pollfd poll_fds[POLL_NUM_SOCEKTS] = {
        {.fd = udp_socket, .events = POLLIN, .revents = 0},
        {.fd = tcp_socket, .events = POLLIN, .revents = 0},
    };

    printf("Receiving as '%s' into %s.\n"
           "Send a file with: share send %s <file>...\n"
           "Press Ctrl-C to stop.\n",
           share_name.c_str(), std::filesystem::current_path().string().c_str(), share_name.c_str());

    while (true) {
        const int num_events = socket_poll(poll_fds, POLL_NUM_SOCEKTS);
        if (num_events <= 0) {
            continue;
        }

        if (poll_fds[POLL_UDP_SOCKET_INDEX].revents & POLLIN) {
            // Receiving packet on UDP, check if it's a proper discovery.
            sockaddr_in discovery_sender_addr {};

            std::vector<char> buffer {};
            buffer.resize(DISCOVERY_REQUEST_SIZE + 1 /* to detect oversized packets */);

            const size_t discovery_request_size =
                socket_recv_from(udp_socket, buffer.data(), buffer.size(), &discovery_sender_addr);

            if (is_valid_discovery_request(buffer.data(), discovery_request_size)) {
                // Valid discovery request, respond with the used TCP port and the share name.
                /*
                 * Scheme of the discovery response.
                 * +--------------------------------------------------------------------------------+
                 * |  8 bytes   |  2 bytes        |  2 bytes           |  <share_name_size> bytes   |
                 * |  <magic>   |  <tcp_port>     |  <share_name_size> |  <share_name>              |
                 * +--------------------------------------------------------------------------------+
                 */

                std::vector<char> discovery_response {};
                discovery_response.resize(DISCOVERY_RESPONSE_HEADER_SIZE + share_name.size());

                const in_port_t tcp_port_network_order = htons(tcp_port);
                const uint16_t share_name_size_network_order = htons(share_name.size());

                char* const discovery_response_data = discovery_response.data() + DISCOVERY_RESPONSE_MAGIC_SIZE;

                memcpy(discovery_response.data(), DISCOVERY_RESPONSE_MAGIC, DISCOVERY_RESPONSE_MAGIC_SIZE);
                memcpy(discovery_response_data, &tcp_port_network_order, 2);
                memcpy(discovery_response_data + 2, &share_name_size_network_order, 2);
                memcpy(discovery_response_data + 4, share_name.c_str(), share_name.size());

                socket_send_from(udp_socket, discovery_response.data(), discovery_response.size(),
                                 &discovery_sender_addr);
            }
        }

        if (poll_fds[POLL_TCP_SOCKET_INDEX].revents & POLLIN) {
            // Receiving packet on TCP.
            /*
             * Scheme of the packet of a single transfer.
             * +-------------------------------------------------------------------------------------------------------+
             * |   1 bytes          |  2 bytes      |  <path_size> bytes   |  8 bytes         |  <content_size> bytes  |
             * |   <type> (f/d/z)   |  <path_size>  |  <path>              |  <content_size>  |  <content>             |
             * +-------------------------------------------------------------------------------------------------------+
             */
            sockaddr_in tcp_sender_socket_addr {};
            const int tcp_sender_socket = socket_accept(tcp_socket, &tcp_sender_socket_addr);

            printf("\nTransfer with %s:%s has started.\n\n", addr_to_str(tcp_sender_socket_addr.sin_addr).c_str(),
                   port_to_str(tcp_sender_socket_addr.sin_port).c_str());

            std::vector<char> buffer {};
            buffer.resize(TRANSFER_CHUNK_SIZE);

            bool connection_closed = false;

            while (!connection_closed) {
                // 1. File type ('f': file, 'd': directory, 'z': end-of-stream).
                char transfer_type {};

                if (!socket_recv_all(tcp_sender_socket, &transfer_type, 1)) {
                    printf("Connection closed by remote peer\n");
                    break;
                }

                if (transfer_type == TRANSFER_TYPE_END) {
                    // End of stream: send ACK.
                    socket_send(tcp_sender_socket, &TRANSFER_TYPE_END_ACK, 1);
                    break;
                }

                // 2. File path size.
                uint16_t file_path_size_network_order {};
                if (!socket_recv_all(tcp_sender_socket, &file_path_size_network_order, 2)) {
                    printf("Connection closed by remote peer\n");
                    break;
                }

                const uint16_t file_path_size = ntohs(file_path_size_network_order);

                // 3. File path.
                std::string file_path {};
                file_path.resize(file_path_size);

                if (!socket_recv_all(tcp_sender_socket, file_path.data(), file_path_size)) {
                    printf("Connection closed by peer\n");
                    break;
                }

                // Ensure the resulting path is under the current path,
                const std::filesystem::path local_file_path = std::filesystem::current_path() / file_path;

                if (!is_path_under(local_file_path, std::filesystem::current_path())) {
                    printf("Path is not under current path: %s\n", local_file_path.string().c_str());
                    break;
                }

                if (transfer_type == TRANSFER_TYPE_DIR) {
                    // Transfer of directory: just create it recursively.
                    std::filesystem::create_directories(local_file_path);
                    continue;
                }

                // 4. Content size
                uint64_t file_size_network_order {};
                if (!socket_recv_all(tcp_sender_socket, &file_size_network_order, 8)) {
                    printf("Connection closed by peer\n");
                    break;
                }

                const uint64_t file_size = ntohll(file_size_network_order);

                // Transfer of file: open the file for writing.
                FILE* const f = fopen(local_file_path.c_str(), "w+");
                if (!f) {
                    perror("fopen");
                    break;
                }

                // 5. File content
                size_t bytes_received_so_far {};

                Progress progress = init_progress(file_path, file_size);
                if (file_size) {
                    do {
                        const size_t bytes_to_read = std::min(buffer.size(), file_size - bytes_received_so_far);

                        size_t read_result = socket_recv(tcp_sender_socket, buffer.data(), bytes_to_read);
                        if (read_result == 0) {
                            connection_closed = true;
                            break;
                        }

                        bytes_received_so_far += read_result;

                        fwrite(buffer.data(), 1, read_result, f);

                        render_progress(progress, bytes_received_so_far);
                    } while (bytes_received_so_far < file_size);
                }

                finish_progress(progress);

                fclose(f);
            }

            printf("\nTransfer with %s:%s has closed.\n", addr_to_str(tcp_sender_socket_addr.sin_addr).c_str(),
                   port_to_str(tcp_sender_socket_addr.sin_port).c_str());

            close(tcp_sender_socket);
        }
    }

    close(udp_socket);
    close(tcp_socket);
}
