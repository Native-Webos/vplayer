// SPDX-License-Identifier: GPL-3.0-or-later
#include "network.hpp"

#include "wnk/log.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <thread>

#if VPLAYER_HAVE_LIBVLC
#include <vlc/vlc.h>
#endif

namespace vplayer {

namespace {

constexpr const char *kCat = "network";
constexpr const char *kSavedKey = "net.places";

std::string lower(std::string s) {
    for (auto &c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string percent_decode(const std::string &s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size() && std::isxdigit(static_cast<unsigned char>(s[i + 1])) &&
            std::isxdigit(static_cast<unsigned char>(s[i + 2]))) {
            out += static_cast<char>(std::strtol(s.substr(i + 1, 2).c_str(), nullptr, 16));
            i += 2;
        } else {
            out += s[i];
        }
    }
    return out;
}

void sort_entries(std::vector<NetEntry> *v) {
    std::stable_sort(v->begin(), v->end(), [](const NetEntry &a, const NetEntry &b) {
        const bool fa = a.kind == NetEntry::Kind::Folder || a.kind == NetEntry::Kind::Server;
        const bool fb = b.kind == NetEntry::Kind::Folder || b.kind == NetEntry::Kind::Server;
        if (fa != fb) return fa;
        return wnk::platform::natural_less(a.name, b.name);
    });
}

}  // namespace

// ---- helpers ----

std::string mrl_protocol(const std::string &mrl) {
    const size_t p = mrl.find("://");
    if (p == std::string::npos) return "FILE";
    std::string s = lower(mrl.substr(0, p));
    if (s == "upnp") return "DLNA";
    for (auto &c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

std::string mrl_display_name(const std::string &mrl) {
    const size_t p = mrl.find("://");
    std::string rest = p == std::string::npos ? mrl : mrl.substr(p + 3);
    while (!rest.empty() && rest.back() == '/') rest.pop_back();
    const size_t q = rest.find_first_of("?#");
    if (q != std::string::npos) rest.resize(q);
    const size_t slash = rest.rfind('/');
    std::string name = slash == std::string::npos ? rest : rest.substr(slash + 1);
    const size_t at = name.rfind('@');  // user@host at a server's root
    if (slash == std::string::npos && at != std::string::npos) name = name.substr(at + 1);
    return percent_decode(name);
}

std::string mrl_parent(const std::string &mrl) {
    const size_t p = mrl.find("://");
    if (p == std::string::npos) return {};
    std::string s = mrl;
    while (s.size() > p + 3 && s.back() == '/') s.pop_back();
    const size_t slash = s.rfind('/');
    if (slash == std::string::npos || slash < p + 3) return {};  // a server's root
    return s.substr(0, slash + 1);
}

bool looks_like_url(const std::string &text) {
    const size_t p = text.find("://");
    if (p == std::string::npos || p == 0 || p + 3 >= text.size()) return false;
    for (size_t i = 0; i < p; ++i) {
        const char c = text[i];
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '+' && c != '-' && c != '.') return false;
    }
    return text.find(' ') == std::string::npos;
}

NetEntry::Kind net_kind(const std::string &name, bool folder) {
    if (folder) return NetEntry::Kind::Folder;
    switch (wnk::platform::file_kind(name)) {
        case wnk::platform::FileKind::Video: return NetEntry::Kind::Video;
        case wnk::platform::FileKind::Audio: return NetEntry::Kind::Audio;
        case wnk::platform::FileKind::Subtitle: return NetEntry::Kind::Subtitle;
        case wnk::platform::FileKind::Image:
        case wnk::platform::FileKind::Playlist: return NetEntry::Kind::Other;
        default: break;
    }
    // DLNA servers name items by title, without an extension: playable as media.
    return name.find('.') == std::string::npos ? NetEntry::Kind::Media : NetEntry::Kind::Other;
}

wnk::platform::FileEntry to_file_entry(const NetEntry &e) {
    wnk::platform::FileEntry f;
    f.name = e.name;
    f.path = e.mrl;
    f.kind = e.kind == NetEntry::Kind::Audio ? wnk::platform::FileKind::Audio : wnk::platform::FileKind::Video;
    return f;
}

// ---- saved places ----

std::string saved_places_json(const SavedPlaces &places) {
    nlohmann::json j;
    j["servers"] = nlohmann::json::array();
    for (const auto &s : places.servers) j["servers"].push_back({{"name", s.name}, {"mrl", s.mrl}});
    j["urls"] = places.urls;
    return j.dump();
}

SavedPlaces saved_places_from_json(const std::string &text) {
    SavedPlaces p;
    const auto j = nlohmann::json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return p;
    if (auto it = j.find("servers"); it != j.end() && it->is_array()) {
        for (const auto &s : *it) {
            if (!s.is_object() || !s.contains("mrl") || !s["mrl"].is_string()) continue;
            NetEntry e;
            e.mrl = s["mrl"].get<std::string>();
            e.name = s.contains("name") && s["name"].is_string() ? s["name"].get<std::string>() : mrl_display_name(e.mrl);
            e.kind = NetEntry::Kind::Server;
            e.detail = mrl_protocol(e.mrl) + " · saved";
            p.servers.push_back(std::move(e));
        }
    }
    if (auto it = j.find("urls"); it != j.end() && it->is_array()) {
        for (const auto &u : *it)
            if (u.is_string() && p.urls.size() < 10) p.urls.push_back(u.get<std::string>());
    }
    return p;
}

void remember_url(SavedPlaces *places, const std::string &url) {
    auto &u = places->urls;
    u.erase(std::remove(u.begin(), u.end(), url), u.end());
    u.insert(u.begin(), url);
    if (u.size() > 10) u.resize(10);
}

SavedPlaces load_saved_places(wnk::storage::KeyValueStore *store) {
    if (store == nullptr) return {};
    wnk::storage::Entry e;
    if (store->get(kSavedKey, &e) != wnk::storage::Status::Ok) return {};
    return saved_places_from_json(e.value);
}

bool save_saved_places(wnk::storage::KeyValueStore *store, const SavedPlaces &places) {
    return store != nullptr && store->put(kSavedKey, saved_places_json(places)).status == wnk::storage::Status::Ok;
}

// ---- browsers ----

namespace {

// Made-up servers for builds without libVLC.
class DemoBrowser final : public NetworkBrowser {
 public:
    std::vector<NetEntry> servers() override {
        return {{"Living room NAS", "smb://nas/", NetEntry::Kind::Server, "SMB · found on this network"},
                {"Media server", "upnp://demo/", NetEntry::Kind::Server, "DLNA media server"}};
    }
    bool list(const std::string &mrl, std::vector<NetEntry> *out, std::string *error,
              const wnk::async::CancellationToken &) override {
        out->clear();
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        auto add = [&](const std::string &name, bool folder) {
            out->push_back({name, mrl + name + (folder ? "/" : ""), net_kind(name, folder), {}});
        };
        if (mrl == "smb://nas/") {
            add("Films", true);
            add("Series", true);
            add("Music", true);
        } else if (mrl == "smb://nas/Films/") {
            add("Big Buck Bunny.mkv", false);
            add("Sintel.mkv", false);
            add("Tears of Steel.mp4", false);
            add("Sintel.srt", false);
        } else if (mrl == "smb://nas/Series/") {
            add("Season 1", true);
        } else if (mrl == "upnp://demo/") {
            add("Videos", true);
            add("Music", true);
        } else if (mrl.rfind("smb://locked/", 0) == 0) {
            *error = "Sign-in needed";
            return false;
        }
        sort_entries(out);
        return true;
    }
};

#if VPLAYER_HAVE_LIBVLC

std::string take(char *s) {
    std::string out = s ? s : "";
    if (s) libvlc_free(s);
    return out;
}

class VlcBrowser final : public NetworkBrowser {
 public:
    explicit VlcBrowser(libvlc_instance_t *vlc) : vlc_(vlc) {
        libvlc_media_discoverer_description_t **list = nullptr;
        const ssize_t n = libvlc_media_discoverer_list_get(vlc_, libvlc_media_discoverer_lan, &list);
        for (ssize_t i = 0; i < n; ++i) {
            const std::string name = list[i]->psz_name ? list[i]->psz_name : "";
            libvlc_media_discoverer_t *md = libvlc_media_discoverer_new(vlc_, name.c_str());
            if (md == nullptr) continue;
            if (libvlc_media_discoverer_start(md) != 0) {
                libvlc_media_discoverer_release(md);
                continue;
            }
            WNK_LOGI(kCat, "discovery: %s (%s)", name.c_str(), list[i]->psz_longname ? list[i]->psz_longname : "");
            discoverers_.push_back({name, md});
        }
        if (n > 0) libvlc_media_discoverer_list_release(list, static_cast<size_t>(n));
    }
    ~VlcBrowser() override {
        for (auto &d : discoverers_) {
            libvlc_media_discoverer_stop(d.md);
            libvlc_media_discoverer_release(d.md);
        }
    }

    std::vector<NetEntry> servers() override {
        std::lock_guard<std::mutex> lock(mu_);
        std::vector<NetEntry> out;
        for (auto &d : discoverers_) {
            libvlc_media_list_t *ml = libvlc_media_discoverer_media_list(d.md);
            if (ml == nullptr) continue;
            libvlc_media_list_lock(ml);
            const int n = libvlc_media_list_count(ml);
            for (int i = 0; i < n; ++i) {
                libvlc_media_t *m = libvlc_media_list_item_at_index(ml, i);
                if (m == nullptr) continue;
                NetEntry e;
                e.mrl = take(libvlc_media_get_mrl(m));
                e.name = take(libvlc_media_get_meta(m, libvlc_meta_Title));
                if (e.name.empty()) e.name = mrl_display_name(e.mrl);
                e.kind = NetEntry::Kind::Server;
                const std::string proto = mrl_protocol(e.mrl);
                e.detail = proto == "DLNA" ? "DLNA media server" : proto + " · found on this network";
                libvlc_media_release(m);
                if (!e.mrl.empty()) out.push_back(std::move(e));
            }
            libvlc_media_list_unlock(ml);
            libvlc_media_list_release(ml);
        }
        sort_entries(&out);
        out.erase(std::unique(out.begin(), out.end(), [](const NetEntry &a, const NetEntry &b) { return a.mrl == b.mrl; }),
                  out.end());
        return out;
    }

    bool list(const std::string &mrl, std::vector<NetEntry> *out, std::string *error,
              const wnk::async::CancellationToken &cancel) override {
        out->clear();
        libvlc_media_t *m = libvlc_media_new_location(vlc_, mrl.c_str());
        if (m == nullptr) {
            *error = "libVLC cannot open this address";
            return false;
        }
        // Network access, and dialogs (sign-in) allowed; 30 s at most, cancellable.
        const auto flags = static_cast<libvlc_media_parse_flag_t>(libvlc_media_parse_network | libvlc_media_do_interact);
        libvlc_media_parse_with_options(m, flags, 30000);
        libvlc_media_parsed_status_t st = static_cast<libvlc_media_parsed_status_t>(0);
        for (;;) {
            st = libvlc_media_get_parsed_status(m);
            if (st != 0) break;
            if (cancel.cancelled()) {
                libvlc_media_parse_stop(m);
                libvlc_media_release(m);
                *error = "cancelled";
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        libvlc_media_list_t *subs = libvlc_media_subitems(m);
        const int n = subs ? libvlc_media_list_count(subs) : 0;
        if (subs != nullptr) {
            libvlc_media_list_lock(subs);
            for (int i = 0; i < n; ++i) {
                libvlc_media_t *s = libvlc_media_list_item_at_index(subs, i);
                if (s == nullptr) continue;
                NetEntry e;
                e.mrl = take(libvlc_media_get_mrl(s));
                const std::string title = take(libvlc_media_get_meta(s, libvlc_meta_Title));
                const std::string from_mrl = mrl_display_name(e.mrl);
                e.name = title.empty() ? from_mrl : title;
                const libvlc_media_type_t t = libvlc_media_get_type(s);
                const bool folder = t == libvlc_media_type_directory;
                // The extension decides what a file is: take it from the MRL when the title has none.
                e.kind = net_kind(e.name.find('.') != std::string::npos || folder ? e.name : from_mrl, folder);
                if (t == libvlc_media_type_disc) e.kind = NetEntry::Kind::Media;
                libvlc_media_release(s);
                if (!e.mrl.empty()) out->push_back(std::move(e));
            }
            libvlc_media_list_unlock(subs);
            libvlc_media_list_release(subs);
        }
        libvlc_media_release(m);
        if (out->empty() && st != libvlc_media_parsed_status_done) {
            *error = st == libvlc_media_parsed_status_timeout ? "the server did not answer in time"
                                                              : "the folder could not be read (address, sign-in or share name?)";
            return false;
        }
        sort_entries(out);
        WNK_LOGI(kCat, "list %s: %zu entries", mrl.c_str(), out->size());
        return true;
    }

 private:
    struct Discoverer {
        std::string name;
        libvlc_media_discoverer_t *md;
    };
    libvlc_instance_t *vlc_;
    std::mutex mu_;
    std::vector<Discoverer> discoverers_;
};

#endif

}  // namespace

std::unique_ptr<NetworkBrowser> make_network_browser(libvlc_instance_t *vlc) {
#if VPLAYER_HAVE_LIBVLC
    if (vlc != nullptr) return std::make_unique<VlcBrowser>(vlc);
#else
    (void)vlc;
#endif
    return std::make_unique<DemoBrowser>();
}

}  // namespace vplayer
