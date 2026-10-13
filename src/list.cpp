#include "list.h"

#include <cstdio>

#include "addrutils.h"
#include "constants.h"
#include "discover.h"

void command_list(const int argc, char** argv) {
    printf("%-32s %-32s\n", "NAME", "ADDRESS");
    printf("---------------------------------------------------\n");

    broadcast_discovery_request(
        DISCOVERY_REQUEST_NUM_ATTEMPTS, DISCOVERY_REQUESTS_DELAY, [](const DiscoveredReceiver& receiver) {
            printf("%-32s %s:%s\n", receiver.name.c_str(), addr_to_str(in_addr {receiver.addr}).c_str(),
                   port_to_str(receiver.port).c_str());
            return true;
        });
}
