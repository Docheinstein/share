#ifndef COMMON_H
#define COMMON_H

#include <cstdio>

#include "constants.h"

inline void print_usage(FILE* stream = stdout) {
    fprintf(
        stream,
        "share - send files to another machine on the local network\n"
        "\n"
        "Usage:\n"
        "  share recv [name]                              wait for files, and save them into the current directory.\n"
        "                                                 <name> defaults to the current directory's name.\n"
        "  share send [<name>[@address:port]] <path>...   send files or directories to the receiver <name>.\n"
        "                                                 The <name> can be omitted if there is exactly one\n"
        "                                                 receiver in the network.\n"
        "  share list                                     list the receivers on the network.\n"
        "\n"
        "'s', 'r' and 'l' are shortcuts for 'send', 'recv' and 'list'.\n"
        "If the receiver's address and port are not specified, share figures them out"
        "by sending a discovery packet over the network (on UDP port %u).",
        SHARE_UDP_PORT);
}

#endif // COMMON_H
