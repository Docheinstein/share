#ifndef PROGRESS_H
#define PROGRESS_H

#include <chrono>
#include <cstddef>
#include <string>

struct Progress {
    std::string file_name;
    size_t file_size;
    std::chrono::steady_clock::time_point start_time;

    std::chrono::steady_clock::time_point last_render_time;
    size_t last_transferred_bytes;

    size_t current_speed_update_transferred_bytes;
    std::chrono::steady_clock::time_point current_speed_update_time;
    size_t last_speed_update_transferred_bytes;
    std::chrono::steady_clock::time_point last_speed_update_time;

    bool is_terminal;
};

Progress init_progress(std::string file_name, size_t file_size);
void render_progress(Progress& pr, size_t bytes);
void finish_progress(Progress& pr);

#endif // PROGRESS_H
