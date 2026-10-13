#ifndef PATHUTILS_H
#define PATHUTILS_H

#include <filesystem>
#include <string>

bool is_path_under(const std::filesystem::path& p, const std::filesystem::path& root);

std::string path_to_utf8(const std::filesystem::path& p);
std::filesystem::path path_from_utf8(const std::string& s);

FILE* file_open(const std::filesystem::path& p, const char* mode);

#endif // PATHUTILS_H
