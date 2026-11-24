// SPDX-License-Identifier: GPL-3.0-or-later
#include "music.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <chrono>
#include <map>
#include <numeric>
#include <thread>

#if VPLAYER_HAVE_LIBVLC
#include <vlc/vlc.h>
#endif

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

std::vector<Artist> group_artists(const std::vector<Album> &albums) {
    std::map<std::string, size_t> at;  // the name's index in out
    std::vector<Artist> out;
    for (size_t i = 0; i < albums.size(); ++i) {
        const std::string &name = albums[i].artist;
        if (name.empty()) continue;
        auto [it, added] = at.emplace(name, out.size());
        if (added) out.push_back({name, {}});
        out[it->second].albums.push_back(i);
    }
    std::stable_sort(out.begin(), out.end(), [](const Artist &a, const Artist &b) {
        return wnk::platform::natural_less(a.name, b.name);
    });
    return out;
}

std::vector<size_t> tag_order(const std::vector<TrackTags> &tags) {
    std::vector<size_t> order(tags.size());
    std::iota(order.begin(), order.end(), size_t{0});
    const bool numbered = !tags.empty() && std::all_of(tags.begin(), tags.end(), [](const TrackTags &t) { return t.number > 0; });
    if (numbered)
        std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return tags[a].number < tags[b].number; });
    return order;
}

#if VPLAYER_HAVE_LIBVLC

std::vector<TrackTags> read_tags(libvlc_instance_t *vlc, const std::vector<std::string> &paths,
                                 const std::function<bool()> &stop) {
    std::vector<TrackTags> out(paths.size());
    if (vlc == nullptr) return out;
    auto meta = [](libvlc_media_t *m, libvlc_meta_t k) {
        char *v = libvlc_media_get_meta(m, k);
        std::string s = v ? v : "";
        if (v) libvlc_free(v);
        return s;
    };
    for (size_t i = 0; i < paths.size(); ++i) {
        if (stop && stop()) break;
        libvlc_media_t *m = libvlc_media_new_path(vlc, paths[i].c_str());
        if (m == nullptr) continue;
        libvlc_media_parse_with_options(m, libvlc_media_parse_local, 2000);
        for (int k = 0; k < 220 && libvlc_media_get_parsed_status(m) == 0 && !(stop && stop()); ++k)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        TrackTags &t = out[i];
        t.title = meta(m, libvlc_meta_Title);
        t.artist = meta(m, libvlc_meta_Artist);
        t.album = meta(m, libvlc_meta_Album);
        t.number = std::atoi(meta(m, libvlc_meta_TrackNumber).c_str());
        const libvlc_time_t d = libvlc_media_get_duration(m);
        t.duration_ms = d > 0 ? d : 0;
        // libVLC names untagged files after themselves: no real title then.
        if (t.title == paths[i].substr(paths[i].rfind('/') + 1)) t.title.clear();
        libvlc_media_release(m);
    }
    return out;
}
#else

std::vector<TrackTags> read_tags(libvlc_instance_t *, const std::vector<std::string> &paths,
                                 const std::function<bool()> &) {
    return std::vector<TrackTags>(paths.size());
}
#endif

}  // namespace vplayer
