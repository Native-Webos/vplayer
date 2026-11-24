// SPDX-License-Identifier: GPL-3.0-or-later
#include "music.hpp"

#include <algorithm>
#include <cctype>
#include <map>

namespace vplayer {

namespace {

std::string lower(std::string s) {
    for (auto &c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
std::string folder_of(const std::string &path) {
    const size_t s = path.rfind('/');
    return s == std::string::npos ? std::string() : path.substr(0, s);
}
std::string base(const std::string &path) {
    const size_t s = path.rfind('/');
    return s == std::string::npos ? path : path.substr(s + 1);
}

}  // namespace

std::string pick_cover(const std::vector<std::string> &images) {
    static const char *preferred[] = {"cover", "folder", "front", "albumart"};
    for (const char *p : preferred) {
        for (const auto &img : images) {
            const std::string n = lower(base(img));
            if (n.rfind(p, 0) == 0) return img;
        }
    }
    return images.size() == 1 ? images[0] : std::string();
}

std::vector<Album> build_albums(const std::vector<wnk::platform::FileEntry> &files) {
    std::map<std::string, Album> by_folder;
    std::map<std::string, std::vector<std::string>> images;
    for (const auto &f : files) {
        const std::string folder = folder_of(f.path);
        if (f.kind == wnk::platform::FileKind::Audio) {
            Album &a = by_folder[folder];
            a.folder = folder;
            a.tracks.push_back(f);
        } else if (f.kind == wnk::platform::FileKind::Image) {
            const std::string ext = lower(f.name.substr(f.name.rfind('.') + 1));
            if (ext == "jpg" || ext == "jpeg" || ext == "png") images[folder].push_back(f.path);
        }
    }
    std::vector<Album> out;
    for (auto &[folder, a] : by_folder) {
        a.title = base(folder);
        const std::string parent = folder_of(folder);
        a.artist = parent.empty() ? std::string() : base(parent);
        if (auto it = images.find(folder); it != images.end()) a.cover = pick_cover(it->second);
        std::sort(a.tracks.begin(), a.tracks.end(), [](const auto &x, const auto &y) {
            return wnk::platform::natural_less(x.name, y.name);
        });
        out.push_back(std::move(a));
    }
    std::sort(out.begin(), out.end(), [](const Album &x, const Album &y) {
        if (x.artist != y.artist) return wnk::platform::natural_less(x.artist, y.artist);
        return wnk::platform::natural_less(x.title, y.title);
    });
    return out;
}

}  // namespace vplayer
