#ifndef CONSTANTS_H
#define CONSTANTS_H

#include <cstddef>
#include <cstdint>
#include <netinet/in.h>

constexpr int SHARE_UDP_PORT = 47000;
constexpr int SHARE_TCP_MIN_PORT = 47001;
constexpr int SHARE_TCP_MAX_PORT = 47101;

constexpr uint32_t FILE_NAME_MAX_LENGTH = 1024;
constexpr uint32_t SHARE_NAME_MAX_LENGTH = 1024;

constexpr char DISCOVERY_REQUEST_DATA[8] = {'S', 'H', 'R', 'D', 'S', 'C', 'R', 'Q'};
constexpr size_t DISCOVERY_REQUEST_SIZE = sizeof(DISCOVERY_REQUEST_DATA);
constexpr uint8_t DISCOVERY_REQUEST_NUM_ATTEMPTS = 3;
constexpr int DISCOVERY_REQUESTS_DELAY = 300;

constexpr char DISCOVERY_RESPONSE_MAGIC[8] = {'S', 'H', 'R', 'D', 'S', 'C', 'R', 'S'};
constexpr size_t DISCOVERY_RESPONSE_MAGIC_SIZE = sizeof(DISCOVERY_RESPONSE_MAGIC);
constexpr size_t DISCOVERY_RESPONSE_HEADER_SIZE =
    DISCOVERY_RESPONSE_MAGIC_SIZE + sizeof(in_port_t) + sizeof(uint16_t) /* share name size */;
constexpr size_t DISCOVERY_RESPONSE_MAX_SIZE = DISCOVERY_RESPONSE_HEADER_SIZE + SHARE_NAME_MAX_LENGTH;

static constexpr size_t TRANSFER_CHUNK_SIZE = 1024;

constexpr char TRANSFER_TYPE_FILE = 'f';
constexpr char TRANSFER_TYPE_DIR = 'd';
constexpr char TRANSFER_TYPE_END = 'z';
constexpr char TRANSFER_TYPE_END_ACK = 'k';

constexpr size_t KB = 1024;
constexpr size_t MB = 1024 * 1024;
constexpr size_t GB = 1024 * 1024 * 1024;

#endif // CONSTANTS_H
