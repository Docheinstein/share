#include "pathutils.h"

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
