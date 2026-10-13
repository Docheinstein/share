#include "progress.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <sys/ioctl.h>
#include <unistd.h>
#include <utility>

#include "constants.h"

namespace {
constexpr std::chrono::milliseconds REFRESH_INTERVAL {16};
constexpr std::chrono::milliseconds SPEED_REFRESH_INTERVAL {500};
constexpr size_t MINIMUM_BAR_WIDTH = 10;
constexpr size_t MAXIMUM_BAR_WIDTH = 48;
constexpr size_t MINIMUM_NAME_WIDTH = 16;
constexpr size_t DEFAULT_TERM_COLS = 80;
constexpr char PROGRESS_CHAR = '#';
constexpr char CLEAR_LINE[] = "\r\x1b[K";

uint32_t get_term_cols() {
    winsize ws {};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) {
        return ws.ws_col;
    }

    return DEFAULT_TERM_COLS;
}

std::string size_to_human_string(const size_t bytes) {
    if (bytes < KB) {
        return std::to_string(bytes) + " B";
    }

    double value;
    const char* unit;

    if (bytes < MB) {
        value = static_cast<double>(bytes) / KB;
        unit = "KB";
    } else if (bytes < GB) {
        value = static_cast<double>(bytes) / MB;
        unit = "MB";
    } else {
        value = static_cast<double>(bytes) / GB;
        unit = "GB";
    }

    char buffer[32];
    snprintf(buffer, sizeof(buffer), "%.1f %s", value, unit);
    return buffer;
}

std::string fit_to_width(std::string s, const size_t max_length) {
    const size_t string_size = s.size();

    if (string_size <= max_length) {
        s.resize(max_length, ' ');
        return s;
    }

    if (max_length <= 3) {
        return std::string(max_length, '.');
    }

    return "..." + s.substr(string_size - (max_length - 3));
}

size_t bytes_per_second(const size_t last_bytes, const size_t new_bytes,
                        const std::chrono::steady_clock::time_point last_time,
                        const std::chrono::steady_clock::time_point new_time) {
    if (new_bytes <= last_bytes) {
        return 0;
    }

    const auto delta_bytes = new_bytes - last_bytes;
    const double delta_ns =
        static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(new_time - last_time).count());

    const double bytes_per_ns = static_cast<double>(delta_bytes) / delta_ns;

    return static_cast<size_t>(bytes_per_ns * 1e9);
}

size_t progress_percentage(const Progress& pr, const size_t bytes) {
    return pr.file_size ? bytes * 100 / pr.file_size : 100;
}

void do_progress_internal(Progress& pr, size_t bytes) {
    const bool done = bytes >= pr.file_size;

    pr.last_transferred_bytes = bytes;

    // Throttle the refresh, unless this is either the first or last update.
    const auto now = std::chrono::steady_clock::now();
    if (!done && now - pr.last_render_time < REFRESH_INTERVAL) {
        return;
    }

    pr.last_render_time = now;

    if (now - pr.current_speed_update_time >= SPEED_REFRESH_INTERVAL) {
        pr.last_speed_update_time = pr.current_speed_update_time;
        pr.last_speed_update_transferred_bytes = pr.current_speed_update_transferred_bytes;

        pr.current_speed_update_time = now;
        pr.current_speed_update_transferred_bytes = bytes;
    }

    size_t b0, b1;
    std::chrono::steady_clock::time_point t0, t1;

    if (!done) {
        // Show the speed of the last transfer window (500ms).
        b0 = pr.last_speed_update_transferred_bytes;
        t0 = pr.last_speed_update_time;
        b1 = pr.current_speed_update_transferred_bytes;
        t1 = pr.current_speed_update_time;
    } else {
        // File transfer ended: show average speed instead.
        b0 = 0;
        t0 = pr.start_time;
        b1 = bytes;
        t1 = now;
    }

    const size_t columns = get_term_cols();

    char stats[96];
    const int stats_length =
        snprintf(stats, sizeof(stats), " %3zu%%  %9s / %-9s  %9s/s", progress_percentage(pr, bytes),
                 size_to_human_string(bytes).c_str(), size_to_human_string(pr.file_size).c_str(),
                 size_to_human_string(bytes_per_second(b0, b1, t0, t1)).c_str());

    static constexpr size_t BRACKETS_WIDTH = 4 /* " [" + "] " */;

    const size_t minimum_cols_for_stats_bar_and_name =
        MINIMUM_BAR_WIDTH + MINIMUM_NAME_WIDTH + stats_length + BRACKETS_WIDTH;

    const size_t minimum_cols_for_stats_and_name = MINIMUM_NAME_WIDTH + stats_length;

    std::string line = CLEAR_LINE;

    if (columns < minimum_cols_for_stats_and_name) {
        // Just render file name.
        line += fit_to_width(pr.file_name, columns);
    } else if (columns < minimum_cols_for_stats_bar_and_name) {
        // Render file name and stats (but not bar).
        const size_t name_width = columns - stats_length;

        line += fit_to_width(pr.file_name, name_width);
        line += stats;
    } else {
        // Render file name, stats and bar.
        const size_t bar_and_name_width = columns - stats_length - BRACKETS_WIDTH;
        const size_t maximum_leftover_width = bar_and_name_width - (MINIMUM_NAME_WIDTH + MINIMUM_BAR_WIDTH);
        const size_t bar_width = std::min(MINIMUM_BAR_WIDTH + maximum_leftover_width / 2, MAXIMUM_BAR_WIDTH);
        const size_t name_width = bar_and_name_width - bar_width;

        const size_t num_ticks = pr.file_size ? std::min(bytes * bar_width / pr.file_size, bar_width) : bar_width;

        std::string bar(num_ticks, PROGRESS_CHAR);
        bar.resize(bar_width, ' ');

        line += fit_to_width(pr.file_name, name_width);
        line += " [";
        line += bar;
        line += "] ";
        line += stats;
    }

    fputs(line.c_str(), stdout);
}
} // namespace

Progress init_progress(std::string file_name, const size_t file_size) {
    Progress pr {};
    pr.file_name = std::move(file_name);
    pr.file_size = file_size;
    pr.start_time = std::chrono::steady_clock::now();
    pr.last_render_time = pr.start_time - 2 * REFRESH_INTERVAL; // Force first render.
    pr.last_speed_update_time = pr.start_time;
    pr.last_speed_update_transferred_bytes = 0;
    pr.current_speed_update_time = pr.start_time;
    pr.current_speed_update_transferred_bytes = 0;
    pr.is_terminal = isatty(STDOUT_FILENO);
    return pr;
}

void render_progress(Progress& pr, const size_t bytes) {
    if (!pr.is_terminal) {
        return;
    }

    do_progress_internal(pr, bytes);
    fflush(stdout);
}

void finish_progress(Progress& pr) {
    if (!pr.is_terminal) {
        return;
    }

    do_progress_internal(pr, pr.last_transferred_bytes);
    fputs("\n", stdout);
}
