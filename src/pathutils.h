#ifndef PATHUTILS_H
#define PATHUTILS_H

#include <filesystem>

bool is_path_under(const std::filesystem::path& p, const std::filesystem::path& root);

#endif // PATHUTILS_H
