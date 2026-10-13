#include "platform.h"

#ifdef _WIN32
#include <string>
#include <vector>
#include <windows.h>

#include <shellapi.h>

#ifndef ENABLE_VIRTUAL_TERMINAL_PROCESSING
#define ENABLE_VIRTUAL_TERMINAL_PROCESSING 0x0004
#endif

namespace {
std::string wide_to_utf8(const wchar_t* w) {
    const int size = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(size > 1 ? size - 1 : 0, '\0');
    if (size > 1) {
        WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), size, nullptr, nullptr);
    }
    return s;
}
} // namespace

void platform_init(int& argc, char**& argv) {
    // The narrow argv is in the ANSI code page: rebuild it as UTF-8 from the wide command line.
    static std::vector<std::string> utf8_args;
    static std::vector<char*> utf8_argv;

    int num_args = 0;
    if (wchar_t** const wide_argv = CommandLineToArgvW(GetCommandLineW(), &num_args)) {
        for (int i = 0; i < num_args; ++i) {
            utf8_args.push_back(wide_to_utf8(wide_argv[i]));
        }
        LocalFree(wide_argv);

        for (std::string& arg : utf8_args) {
            utf8_argv.push_back(arg.data());
        }
        utf8_argv.push_back(nullptr);

        argc = num_args;
        argv = utf8_argv.data();
    }

    SetConsoleOutputCP(CP_UTF8);

    const HANDLE stdout_handle = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (GetConsoleMode(stdout_handle, &mode)) {
        SetConsoleMode(stdout_handle, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    }
}
#else
void platform_init(int& /* argc */, char**& /* argv */) {
}
#endif
