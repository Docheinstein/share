#ifndef DISCOVER_H
#define DISCOVER_H

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "net.h"

struct DiscoveredReceiver {
    in_addr_t addr;
    in_port_t port;
    std::string name;
};

std::vector<DiscoveredReceiver> broadcast_discovery_request(
    uint32_t num_attempts, uint32_t delay_between_attempts_ms,
    const std::function<bool(const DiscoveredReceiver&)>& on_receiver_found = [](const DiscoveredReceiver&) {
        return true;
    });

#endif // DISCOVER_H
