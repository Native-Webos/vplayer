// SPDX-License-Identifier: GPL-3.0-or-later
#include "playlist.hpp"

#include "wnk/media/xml.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <sstream>

namespace vplayer {

namespace {

std::string trim(std::string s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

std::string lower(std::string s) {
    for (auto &c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool starts_with(const std::string &s, const char *p) { return s.rfind(p, 0) == 0; }

std::vector<std::string> lines_of(const std::string &text) {
    std::vector<std::string> out;
    std::string line;
    std::istringstream in(text);
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        out.push_back(line);
    }
    if (!out.empty() && out[0].size() >= 3 && static_cast<unsigned char>(out[0][0]) == 0xEF &&
        static_cast<unsigned char>(out[0][1]) == 0xBB && static_cast<unsigned char>(out[0][2]) == 0xBF)
        out[0] = out[0].substr(3);  // UTF-8 byte order mark
    return out;
}

// key="value" attributes of an #EXTINF line (before the title's comma).
std::string attribute(const std::string &s, const std::string &key) {
    const std::string k = key + "=\"";
    size_t p = 0;
    while ((p = s.find(k, p)) != std::string::npos) {
        if (p == 0 || s[p - 1] == ' ' || s[p - 1] == ',') {
            const size_t start = p + k.size();
            const size_t end = s.find('"', start);
            return end == std::string::npos ? std::string() : s.substr(start, end - start);
        }
        p += k.size();
    }
    return {};
}

// The comma that ends #EXTINF's attributes: the first one outside quotes.
size_t title_comma(const std::string &s) {
    bool quoted = false;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '"') quoted = !quoted;
        if (s[i] == ',' && !quoted) return i;
    }
    return std::string::npos;
}

std::string name_of(const std::string &location) {
    std::string s = location;
    while (!s.empty() && s.back() == '/') s.pop_back();
    const size_t q = s.find_first_of("?#", s.find("://") == std::string::npos ? 0 : s.find("://") + 3);
    if (q != std::string::npos) s.resize(q);
    const size_t slash = s.rfind('/');
    return slash == std::string::npos ? s : s.substr(slash + 1);
}

bool parse_m3u(const std::vector<std::string> &lines, const std::string &location, Playlist *out) {
    PlaylistItem pending;
    bool have_info = false;
    std::string group;  // #EXTGRP applies to the entries that follow
    for (const auto &raw : lines) {
        const std::string line = trim(raw);
        if (line.empty()) continue;
        if (starts_with(line, "#EXT-X-TARGETDURATION") || starts_with(line, "#EXT-X-STREAM-INF") ||
            starts_with(line, "#EXT-X-MEDIA-SEQUENCE") || starts_with(line, "#EXT-X-PLAYLIST-TYPE")) {
            out->is_stream = true;
            out->items.clear();
            return true;
        }
        if (starts_with(line, "#EXTINF:")) {
            pending = {};
            const std::string body = line.substr(8);
            const size_t comma = title_comma(body);
            const std::string head = comma == std::string::npos ? body : body.substr(0, comma);
            pending.title = comma == std::string::npos ? std::string() : trim(body.substr(comma + 1));
            const long d = std::strtol(head.c_str(), nullptr, 10);
            pending.duration_ms = d > 0 ? d * 1000 : -1;
            pending.logo = attribute(head, "tvg-logo");
            pending.group = attribute(head, "group-title");
            if (pending.title.empty()) pending.title = attribute(head, "tvg-name");
            have_info = true;
            continue;
        }
        if (starts_with(line, "#EXTGRP:")) {
            group = trim(line.substr(8));
            continue;
        }
        if (line[0] == '#') continue;
        PlaylistItem item = have_info ? pending : PlaylistItem{};
        item.location = resolve_location(line, location);
        if (item.group.empty()) item.group = group;
        if (item.title.empty()) item.title = name_of(line);
        out->items.push_back(std::move(item));
        have_info = false;
    }
    return true;
}

bool parse_pls(const std::vector<std::string> &lines, const std::string &location, Playlist *out, std::string *error) {
    struct Entry {
        std::string file, title;
        int64_t length = -1;
    };
    std::vector<std::pair<int, Entry>> entries;
    auto entry = [&](int n) -> Entry & {
        for (auto &e : entries)
            if (e.first == n) return e.second;
        entries.push_back({n, {}});
        return entries.back().second;
    };
    bool header = false;
    for (const auto &raw : lines) {
        const std::string line = trim(raw);
        if (lower(line) == "[playlist]") {
            header = true;
            continue;
        }
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = lower(line.substr(0, eq)), value = trim(line.substr(eq + 1));
        auto number = [&](const char *prefix) {
            const size_t n = std::char_traits<char>::length(prefix);
            return key.size() > n && key.compare(0, n, prefix) == 0 ? std::atoi(key.c_str() + n) : 0;
        };
        if (int n = number("file")) entry(n).file = value;
        if (int n = number("title")) entry(n).title = value;
        if (int n = number("length")) entry(n).length = std::atol(value.c_str());
    }
    if (!header) {
        *error = "not a PLS playlist";
        return false;
    }
    std::sort(entries.begin(), entries.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
    for (const auto &e : entries) {
        if (e.second.file.empty()) continue;
        PlaylistItem item;
        item.location = resolve_location(e.second.file, location);
        item.title = e.second.title.empty() ? name_of(e.second.file) : e.second.title;
        item.duration_ms = e.second.length > 0 ? e.second.length * 1000 : -1;
        out->items.push_back(std::move(item));
    }
    return true;
}

std::string percent_decode_file_url(const std::string &url) {
    std::string out;
    for (size_t i = 0; i < url.size(); ++i) {
        if (url[i] == '%' && i + 2 < url.size() && std::isxdigit(static_cast<unsigned char>(url[i + 1])) &&
            std::isxdigit(static_cast<unsigned char>(url[i + 2]))) {
            out += static_cast<char>(std::strtol(url.substr(i + 1, 2).c_str(), nullptr, 16));
            i += 2;
        } else {
            out += url[i];
        }
    }
    return out;
}

bool parse_xspf(const std::string &text, const std::string &location, Playlist *out, std::string *error) {
    wnk::media::XmlNode root;
    if (!wnk::media::parse_xml(text, &root, error)) return false;
    const wnk::media::XmlNode *pl = root.local() == "playlist" ? &root : root.child("playlist");
    if (pl == nullptr) {
        *error = "not an XSPF playlist";
        return false;
    }
    const wnk::media::XmlNode *list = pl->child("trackList");
    if (list == nullptr) return true;
    for (const auto *t : list->children_named("track")) {
        const auto *loc = t->child("location");
        if (loc == nullptr || trim(loc->text).empty()) continue;
        PlaylistItem item;
        std::string l = trim(loc->text);
        if (starts_with(l, "file://")) l = percent_decode_file_url(l.substr(7));  // a local path
        item.location = resolve_location(l, location);
        if (const auto *x = t->child("title")) item.title = trim(x->text);
        if (const auto *x = t->child("image")) item.logo = trim(x->text);
        if (const auto *x = t->child("album")) item.group = trim(x->text);
        if (const auto *x = t->child("duration")) item.duration_ms = std::atol(trim(x->text).c_str());
        if (item.duration_ms <= 0) item.duration_ms = -1;
        if (item.title.empty()) item.title = name_of(item.location);
        out->items.push_back(std::move(item));
    }
    return true;
}

}  // namespace

std::vector<std::string> Playlist::groups() const {
    std::vector<std::string> out;
    for (const auto &i : items)
        if (std::find(out.begin(), out.end(), i.group) == out.end()) out.push_back(i.group);
    return out;
}

PlaylistFormat playlist_format(const std::string &name, const std::string &text) {
    std::string n = lower(name);
    const size_t q = n.find_first_of("?#", n.find("://") == std::string::npos ? 0 : n.find("://") + 3);
    if (q != std::string::npos) n.resize(q);
    auto ends = [&](const char *e) {
        const size_t k = std::char_traits<char>::length(e);
        return n.size() >= k && n.compare(n.size() - k, k, e) == 0;
    };
    if (ends(".m3u") || ends(".m3u8")) return PlaylistFormat::M3u;
    if (ends(".pls")) return PlaylistFormat::Pls;
    if (ends(".xspf")) return PlaylistFormat::Xspf;
    const std::string head = lower(trim(text.substr(0, 512)));
    if (starts_with(head, "#extm3u") || starts_with(head, "\xef\xbb\xbf#extm3u")) return PlaylistFormat::M3u;
    if (starts_with(head, "[playlist]")) return PlaylistFormat::Pls;
    if (head.find("<playlist") != std::string::npos && head.find("xspf") != std::string::npos) return PlaylistFormat::Xspf;
    return PlaylistFormat::Unknown;
}

std::string resolve_location(const std::string &entry, const std::string &base) {
    if (entry.empty()) return entry;
    if (entry.find("://") != std::string::npos || entry[0] == '/') return entry;  // absolute
    if (base.find("://") != std::string::npos) {
        const size_t scheme_end = base.find("://") + 3;
        std::string b = base;
        const size_t q = b.find_first_of("?#", scheme_end);
        if (q != std::string::npos) b.resize(q);
        const size_t slash = b.rfind('/');
        if (slash == std::string::npos || slash < scheme_end) return b + "/" + entry;
        return b.substr(0, slash + 1) + entry;
    }
    const size_t slash = base.rfind('/');
    std::string e = entry;
    for (auto &c : e)
        if (c == '\\') c = '/';  // Windows-made playlists
    return slash == std::string::npos ? e : base.substr(0, slash + 1) + e;
}

std::string saved_playlists_json(const std::vector<SavedPlaylist> &lists) {
    nlohmann::json j = nlohmann::json::array();
    for (const auto &l : lists) j.push_back({{"name", l.name}, {"url", l.url}});
    return j.dump();
}

std::vector<SavedPlaylist> saved_playlists_from_json(const std::string &text) {
    std::vector<SavedPlaylist> out;
    const auto j = nlohmann::json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_array()) return out;
    for (const auto &x : j) {
        if (!x.is_object() || !x.contains("url") || !x["url"].is_string()) continue;
        out.push_back({x.value("name", std::string()), x["url"].get<std::string>()});
    }
    return out;
}

std::string my_playlists_json(const std::vector<MyPlaylist> &lists) {
    nlohmann::json j = nlohmann::json::array();
    for (const auto &l : lists) {
        nlohmann::json items = nlohmann::json::array();
        for (const auto &i : l.items) items.push_back({{"t", i.title}, {"l", i.location}});
        j.push_back({{"name", l.name}, {"items", items}});
    }
    return j.dump();
}

std::vector<MyPlaylist> my_playlists_from_json(const std::string &text) {
    std::vector<MyPlaylist> out;
    const auto j = nlohmann::json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_array()) return out;
    for (const auto &x : j) {
        if (!x.is_object() || !x.contains("name") || !x["name"].is_string()) continue;
        MyPlaylist l;
        l.name = x["name"].get<std::string>();
        if (auto it = x.find("items"); it != x.end() && it->is_array())
            for (const auto &i : *it)
                if (i.is_object() && i.contains("l") && i["l"].is_string())
                    l.items.push_back({i.value("t", std::string()), i["l"].get<std::string>(), {}, {}, -1});
        out.push_back(std::move(l));
    }
    return out;
}

bool my_playlists_add(std::vector<MyPlaylist> *lists, const std::string &name, const PlaylistItem &item) {
    auto it = std::find_if(lists->begin(), lists->end(), [&](const MyPlaylist &l) { return l.name == name; });
    if (it == lists->end()) {
        lists->push_back({name, {}});
        it = lists->end() - 1;
    }
    if (std::any_of(it->items.begin(), it->items.end(), [&](const PlaylistItem &i) { return i.location == item.location; }))
        return false;
    it->items.push_back(item);
    return true;
}

bool parse_playlist(const std::string &text, const std::string &location, Playlist *out, std::string *error) {
    *out = {};
    switch (playlist_format(location, text)) {
        case PlaylistFormat::M3u: return parse_m3u(lines_of(text), location, out);
        case PlaylistFormat::Pls: return parse_pls(lines_of(text), location, out, error);
        case PlaylistFormat::Xspf: return parse_xspf(text, location, out, error);
        default: break;
    }
    // No extension, no header: a bare list of addresses is M3U.
    const auto lines = lines_of(text);
    const bool addresses = std::any_of(lines.begin(), lines.end(), [](const std::string &l) {
        return l.find("://") != std::string::npos && l.find(' ') == std::string::npos;
    });
    if (addresses) return parse_m3u(lines, location, out);
    *error = "not a playlist Pylon reads (M3U, PLS or XSPF)";
    return false;
}

}  // namespace vplayer
