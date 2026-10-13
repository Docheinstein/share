#include "pathutils.h"

#include <cstring>

bool is_path_under(const std::filesystem::path& p, const std::filesystem::path& root) {
    const std::filesystem::path p_abs = std::filesystem::weakly_canonical(p);
    const std::filesystem::path r_abs = std::filesystem::weakly_canonical(root);

    auto p_it = p_abs.begin();
    auto r_it = r_abs.begin();

    while (p_it != p_abs.end() && r_it != r_abs.end() && *p_it == *r_it) {
        ++p_it;
        ++r_it;
    }

    return r_it == r_abs.end() && p_it != p_abs.end();
}

std::string path_to_utf8(const std::filesystem::path& p) {
    const std::u8string s = p.generic_u8string();
    return {s.begin(), s.end()};
}

std::filesystem::path path_from_utf8(const std::string& s) {
    return {std::u8string(s.begin(), s.end())};
}

FILE* file_open(const std::filesystem::path& p, const char* const mode) {
#ifdef _WIN32
    const std::wstring wide_mode(mode, mode + strlen(mode));
    return _wfopen(p.c_str(), wide_mode.c_str());
#else
    return fopen(p.c_str(), mode);
#endif
}
