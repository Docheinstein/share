#include "discover.h"

#include <chrono>
#include <cstring>
#include <ranges>
#include <unordered_map>

#include "constants.h"
#include "sockutils.h"

namespace {
uint64_t compute_receiver_uid(const in_addr addr, const in_port_t port) {
    return static_cast<uint64_t>(addr.s_addr) << 32 | port;
}

bool is_valid_discovery_response(const void* const data, const size_t size) {
    if (size < DISCOVERY_RESPONSE_HEADER_SIZE || size > DISCOVERY_RESPONSE_MAX_SIZE ||
        memcmp(data, DISCOVERY_RESPONSE_MAGIC, DISCOVERY_RESPONSE_MAGIC_SIZE) != 0) {
        return false;
    }

    return true;
}
} // namespace

std::vector<DiscoveredReceiver>
broadcast_discovery_request(const uint32_t num_attempts, const uint32_t delay_between_attempts_ms,
                            const std::function<bool(const DiscoveredReceiver&)>& on_receiver_found) {

    // Create a UDP broadcasting socket.
    const socket_t udp_socket = socket_create_udp();
    socket_set_option(udp_socket, SO_BROADCAST, 1);

    const sockaddr_in udp_socket_addr =
        socket_create_sockaddr(socket_create_addr(htonl(INADDR_BROADCAST)), htons(SHARE_UDP_PORT));

    std::vector<char> buffer {};
    buffer.resize(DISCOVERY_RESPONSE_MAX_SIZE + 1 /* to detect oversized packets */);

    std::unordered_map<uint64_t, DiscoveredReceiver> discovered_receivers {};

    const auto delay_ms = std::chrono::milliseconds(delay_between_attempts_ms);

    for (uint32_t attempt = 0; attempt < num_attempts; ++attempt) {
        const auto deadline = std::chrono::steady_clock::now() + delay_ms;

        // Send discovery packet.
        /*
         * Scheme of the discovery request.
         * +------------+
         * |  8 bytes   |
         * |  <magic>   |
         * +------------+
         */
        socket_send_from(udp_socket, DISCOVERY_REQUEST_DATA, sizeof(DISCOVERY_REQUEST_DATA), &udp_socket_addr);

        while (true) {
            const auto now = std::chrono::steady_clock::now();
            if (now > deadline) {
                break;
            }

            int left_ms =
                static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());

            const bool any_answer = socket_wait_until(udp_socket, left_ms);

            if (any_answer) {
                // Someone answered to the discovery.
                /*
                 * Scheme of the discovery response.
                 * +--------------------------------------------------------------------------------+
                 * |  8 bytes   |  2 bytes        |  2 bytes           |  <share_name_size> bytes   |
                 * |  <magic>   |  <tcp_port>     |  <share_name_size> |  <share_name>              |
                 * +--------------------------------------------------------------------------------+
                 */
                sockaddr_in discovery_response_addr {};
                const size_t discovery_response_size =
                    socket_recv_from(udp_socket, buffer.data(), buffer.size(), &discovery_response_addr);

                // Discard datagrams that are too short, too long or not a discovery response.
                if (!is_valid_discovery_response(buffer.data(), discovery_response_size)) {
                    continue;
                }

                const char* const discovery_response_data = buffer.data() + DISCOVERY_RESPONSE_MAGIC_SIZE;

                const in_addr discovered_addr = discovery_response_addr.sin_addr;

                in_port_t discovered_port;
                memcpy(&discovered_port, discovery_response_data, 2);

                uint16_t share_name_size_network_order;
                memcpy(&share_name_size_network_order, discovery_response_data + 2, 2);

                const uint16_t share_name_size = ntohs(share_name_size_network_order);

                // Discard responses whose declared share name exceeds the received datagram.
                if (DISCOVERY_RESPONSE_HEADER_SIZE + share_name_size > discovery_response_size) {
                    continue;
                }

                const uint64_t receiver_uid = compute_receiver_uid(discovered_addr, discovered_port);
                if (!discovered_receivers.contains(receiver_uid)) {
                    // Add a new receiver to the list.
                    std::string share_name {discovery_response_data + 4, share_name_size};
                    DiscoveredReceiver receiver {.addr = discovery_response_addr.sin_addr, //
                                                 .port = discovered_port,                  //
                                                 .name = std::move(share_name)};
                    discovered_receivers.emplace(receiver_uid, receiver);

                    if (!on_receiver_found(receiver)) {
                        // Abort discovery.
                        break;
                    }
                }
            }
        }
    }

    return discovered_receivers | std::views::values | std::ranges::to<std::vector>();
}
