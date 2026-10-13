#include "send.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <ranges>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "addrutils.h"
#include "common.h"
#include "constants.h"
#include "discover.h"
#include "pathutils.h"
#include "progress.h"
#include "sockutils.h"

namespace {
void send_path(const socket_t socket, const std::filesystem::path& path, const std::filesystem::path& root) {
    /*
     * Scheme of the packet of a single transfer.
     * +--------------------------------------------------------------------------------------------------------+
     * |   1 bytes          |  2 bytes      |  <path_size> bytes   |  8 bytes         |  <content_size> bytes   |
     * |   <type> (f/d/z)   |  <path_size>  |  <path>              |  <content_size>  |  <content>              |
     * +--------------------------------------------------------------------------------------------------------+
     */

    const std::string relative_path = path_to_utf8(
        std::filesystem::proximate(std::filesystem::weakly_canonical(path), std::filesystem::weakly_canonical(root)));
    const size_t relative_path_length = relative_path.size();

    if (relative_path_length > FILE_NAME_MAX_LENGTH) {
        fprintf(stderr, "Skipping %s: name is too long\n", relative_path.c_str());
        return;
    }

    char transfer_type;

    if (std::filesystem::is_regular_file(path)) {
        transfer_type = TRANSFER_TYPE_FILE;
    } else if (std::filesystem::is_directory(path)) {
        transfer_type = TRANSFER_TYPE_DIR;
    } else {
        fprintf(stderr, "Skipping %s: neither a file nor a directory\n", relative_path.c_str());
        return;
    }

    // 1. File type ('f': file, 'd': directory).
    socket_send(socket, &transfer_type, 1);

    // 2. File path size.
    const uint16_t relative_path_length_network_order = htons(relative_path_length);
    socket_send(socket, &relative_path_length_network_order, 2);

    // 3. File path.
    socket_send(socket, relative_path.c_str(), relative_path_length);

    if (transfer_type == TRANSFER_TYPE_FILE) {
        // Open file.f
        FILE* const f = file_open(path, "rb");
        if (!f) {
            perror("open");
            return;
        }

        const uint64_t file_size = std::filesystem::file_size(path);
        const uint64_t file_size_network_order = htonll(file_size);

        // 4. File content size.
        socket_send(socket, &file_size_network_order, 8);

        Progress progress = init_progress(relative_path, file_size);

        // 5. File content.
        if (file_size) {
            // Send the file content in chunks.

            std::vector<char> buffer;
            buffer.resize(TRANSFER_CHUNK_SIZE);

            size_t bytes_read_so_far {};
            size_t read_result {};
            do {
                read_result = fread(buffer.data(), 1, TRANSFER_CHUNK_SIZE, f);
                if (read_result > 0) {
                    bytes_read_so_far += read_result;

                    render_progress(progress, bytes_read_so_far);
                    socket_send(socket, buffer.data(), read_result);
                }
            } while (read_result && bytes_read_so_far != file_size);
        }

        // Close file.
        fclose(f);

        finish_progress(progress);
    }
}

void send_path(const socket_t socket, const std::filesystem::path& path) {
    // Send the file or directory.
    send_path(socket, path, path.parent_path());

    // For directories, send all the files recursively.
    if (std::filesystem::is_directory(path)) {
        for (auto const& entry : std::filesystem::recursive_directory_iterator {path}) {
            send_path(socket, entry, path.parent_path());
        }
    }
}

void send_paths(const in_addr address, const in_port_t port, const std::vector<std::filesystem::path>& paths) {
    // Create TCP socket.
    const socket_t tcp_socket = socket_create_tcp();
    const sockaddr_in tcp_socket_addr = socket_create_sockaddr(address, port);
    socket_connect(tcp_socket, tcp_socket_addr);

    for (const auto& path : paths) {
        send_path(tcp_socket, path);
    }

    // Send the end-of-stream byte and shutdown the socket.
    socket_send(tcp_socket, &TRANSFER_TYPE_END, 1);
    socket_shutdown_write(tcp_socket);

    // Wait for the acknowledgement.
    char ack_byte {};
    socket_recv_all(tcp_socket, &ack_byte, 1);

    if (ack_byte != TRANSFER_TYPE_END_ACK) {
        fprintf(stderr, "Connection with remote has closed unexpectedly\n");
    }

    // Finally close the socket.
    socket_close(tcp_socket);
}

std::pair<in_addr, in_port_t> resolver_receiver_identifier(const std::string& receiver_identifier) {
    const size_t at = receiver_identifier.find('@');
    if (at != std::string::npos) {
        // Receiver identifier is supposed to be: <name>@<address:ip>.
        const std::string receiver_name = receiver_identifier.substr(0, at);
        const std::string receiver_address_and_port = receiver_identifier.substr(at + 1);

        const size_t colon = receiver_address_and_port.find(':');
        if (colon == std::string::npos) {
            fprintf(stderr, "Invalid address: %s\n", receiver_identifier.c_str());
            exit(1);
        }

        const std::string receiver_address_str = receiver_address_and_port.substr(0, colon);
        const std::string receiver_port_str = receiver_address_and_port.substr(colon + 1);

        return {str_to_addr(receiver_address_str), str_to_port(receiver_port_str)};
    }

    // A discover request is needed to resolver the receiver identifier.
    bool discovery_success = false;
    std::pair<in_addr, in_port_t> addr_and_port {};

    broadcast_discovery_request(
        DISCOVERY_REQUEST_NUM_ATTEMPTS, DISCOVERY_REQUESTS_DELAY,
        [&receiver_identifier, &addr_and_port, &discovery_success](const DiscoveredReceiver& receiver) {
            if (receiver_identifier == receiver.name) {
                addr_and_port = std::make_pair(receiver.addr, receiver.port);
                discovery_success = true;
                return false;
            }
            return true;
        });

    if (!discovery_success) {
        fprintf(stderr, "Failed to resolve receiver: %s\n", receiver_identifier.c_str());
        exit(1);
    }

    return addr_and_port;
}
} // namespace

void command_send(int argc, char** argv) {
    if (argc >= 2) {
        // share <target> <file>...
        const auto& [receiver_address, receiver_port] = resolver_receiver_identifier(argv[0]);

        // Multiple files can be transferred.
        const auto paths = std::span(argv + 1, argc - 1) | std::views::transform([](const char* arg) {
                               return std::filesystem::weakly_canonical(path_from_utf8(arg));
                           }) |
                           std::ranges::to<std::vector>();

        send_paths(receiver_address, receiver_port, paths);
    } else if (argc == 1) {
        // share <file>
        // No receiver has been specified, proceed only if exactly one receiver is found on the network.
        std::vector<DiscoveredReceiver> receivers =
            broadcast_discovery_request(DISCOVERY_REQUEST_NUM_ATTEMPTS, DISCOVERY_REQUESTS_DELAY);
        if (receivers.size() == 1) {
            const DiscoveredReceiver& unique_receiver = receivers[0];
            send_paths(unique_receiver.addr, unique_receiver.port,
                       {std::filesystem::weakly_canonical(path_from_utf8(argv[0]))});
        } else {
            fprintf(stderr, "Found %zu receivers, please specify the receiver name.\n", receivers.size());
            fprintf(stderr, "Usage: share send <name> %s...\n\n", argv[0]);
            fprintf(stderr, "%-32s %-32s\n", "NAME", "ADDRESS");
            fprintf(stderr, "---------------------------------------------------\n");

            for (const DiscoveredReceiver& receiver : receivers) {
                fprintf(stderr, "%-32s %s:%s\n", receiver.name.c_str(), addr_to_str(receiver.addr).c_str(),
                        port_to_str(receiver.port).c_str());
            }
        }
    } else {
        print_usage();
        exit(1);
    }
}
