// SPDX-License-Identifier: GPL-3.0-or-later
#include "library.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <sys/stat.h>
#include <utility>

namespace vplayer {

namespace platform = wnk::platform;

std::vector<platform::RemovableVolume> media_volumes() {
    std::vector<platform::RemovableVolume> v = platform::removable_volumes();
    if (const char *extra = std::getenv("VPLAYER_EXTRA_ROOT"); extra != nullptr && extra[0] != '\0') {
        struct stat st {};
        if (stat(extra, &st) == 0 && S_ISDIR(st.st_mode)) {
            platform::RemovableVolume t;
            t.path = extra;
            t.name = "Test folder";
            t.fs_type = "folder";
            v.push_back(std::move(t));
        }
    }
    return v;
}

bool same_volumes(const std::vector<platform::RemovableVolume> &a, const std::vector<platform::RemovableVolume> &b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (a[i].path != b[i].path) return false;
    return true;
}

std::vector<platform::FileEntry> recent_videos(const std::vector<platform::RemovableVolume> &volumes, size_t count,
                                               int max_depth, size_t max_entries) {
    std::vector<platform::FileEntry> videos;
    std::deque<std::pair<std::string, int>> queue;
    for (const auto &v : volumes) queue.emplace_back(v.path, 0);
    size_t seen = 0;
    while (!queue.empty() && seen < max_entries) {
        auto [dir, depth] = queue.front();
        queue.pop_front();
        std::vector<platform::FileEntry> entries;
        std::string error;
        if (!platform::list_folder(dir, &entries, &error, max_entries - seen)) continue;
        seen += entries.size();
        for (auto &e : entries) {
            if (e.kind == platform::FileKind::Folder) {
                if (depth + 1 <= max_depth) queue.emplace_back(e.path, depth + 1);
            } else if (e.kind == platform::FileKind::Video) {
                videos.push_back(std::move(e));
            }
        }
    }
    const size_t n = std::min(count, videos.size());
    std::partial_sort(videos.begin(), videos.begin() + static_cast<std::ptrdiff_t>(n), videos.end(),
                      [](const platform::FileEntry &a, const platform::FileEntry &b) {
                          return a.modified_s > b.modified_s;
                      });
    videos.resize(n);
    return videos;
}

std::string format_bytes(uint64_t bytes) {
    static const char *units[] = {"B", "KB", "MB", "GB", "TB"};
    double v = static_cast<double>(bytes);
    size_t u = 0;
    while (v >= 1000 && u + 1 < sizeof units / sizeof *units) {
        v /= 1000;
        ++u;
    }
    char buf[32];
    if (u == 0 || v >= 100) {
        std::snprintf(buf, sizeof buf, "%.0f %s", v, units[u]);
    } else {
        std::snprintf(buf, sizeof buf, "%.1f %s", v, units[u]);
    }
    return buf;
}

std::string extension_upper(const std::string &name) {
    const size_t dot = name.rfind('.');
    if (dot == std::string::npos || dot == 0 || dot + 1 >= name.size()) return {};
    std::string ext = name.substr(dot + 1);
    if (ext.size() > 5) return {};
    for (char &c : ext) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return ext;
}

std::string display_name(const std::string &name) {
    const size_t dot = name.rfind('.');
    if (dot == std::string::npos || dot == 0) return name;
    return name.substr(0, dot);
}

}  // namespace vplayer
