#include <cstdio>
#include <string_view>

#include "common.h"
#include "list.h"
#include "platform.h"
#include "recv.h"
#include "send.h"

int main(int argc, char** argv) {
    platform_init(argc, argv);

    if (argc < 2) {
        print_usage();
        return 0;
    }

    const std::string_view command = argv[1];
    const int cmd_argc = argc - 2;
    char** const cmd_argv = &argv[2];

    if (command == "recv" || command == "r") {
        command_recv(cmd_argc, cmd_argv);
    } else if (command == "send" || command == "s") {
        command_send(cmd_argc, cmd_argv);
    } else if (command == "list" || command == "l") {
        command_list(cmd_argc, cmd_argv);
    } else if (command == "help" || command == "h" || command == "--help" || command == "-h" || command == "-?" ||
               command == "usage" || command == "--usage") {
        print_usage();
    } else {
        fprintf(stderr, "Unknown command '%s'\n", command.data());
        return 1;
    }

    return 0;
}
