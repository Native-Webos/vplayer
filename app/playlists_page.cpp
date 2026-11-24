// SPDX-License-Identifier: GPL-3.0-or-later
#include "playlists_page.hpp"

#include "library.hpp"
#include "network.hpp"

#include "wnk/log.hpp"
#include "wnk/ui/navigation.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <sstream>

namespace vplayer {

using namespace wnk::ui;
using wnk::Size;
namespace input = wnk::input;
namespace platform = wnk::platform;
using vplayer::ui::Chip;
using vplayer::ui::Tile;
using vplayer::ui::TileSpec;
namespace col = vplayer::ui::col;
namespace metric = vplayer::ui::metric;

namespace {

constexpr const char *kCat = "playlists";
constexpr const char *kSavedKey = "playlists.saved";
constexpr size_t kMaxListBytes = 32u << 20;

struct Loaded {
    Playlist list;
    std::string error;
};

bool read_local(const std::string &path, std::string *out, std::string *error) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        *error = "the file cannot be read";
        return false;
    }
    std::ostringstream s;
    s << f.rdbuf();
    *out = s.str();
    if (out->size() > kMaxListBytes) {
        *error = "the list is too large";
        return false;
    }
    return true;
}

std::string duration_text(int64_t ms) { return ms > 0 ? clock_text(ms) : std::string(); }

}  // namespace

PlaylistsPage::PlaylistsPage(Services &services) : s_(services) {
    name = "page.playlists";
    auto &col = add<Column>(24);
    auto &header = col.add<Row>(12);
    header.set_height(Dim::fixed(52));
    crumbs_ = &header.add<Row>(10);
    crumbs_->set_align(Align::Start, Align::Center);
    header.add<View>().set_width(Dim::fill());
    count_ = &header.add<Label>("");
    count_->set_font_size(20);
    count_->set_muted(true);
    count_->set_align(Align::End, Align::Center);

    auto &body = col.add<View>();
    body.set_height(Dim::fill());
    grid_ = &body.add<CollectionView>(Axis::Vertical, 5, Size{metric::tile_w, metric::tile_h}, metric::tile_gap);
    grid_->set_content_padding(Insets{12, 20, 12, 12});
    grid_->name = "playlists.grid";
    grid_->on_activate = [this](Ui &ui, size_t i) { activate(ui, i); };
    message_ = &body.add<Label>("");
    message_->set_font_size(24);
    message_->set_muted(true);
    message_->set_max_lines(3);
    message_->set_text_align(Align::Center);
    message_->set_align(Align::Center, Align::Center);
    message_->set_visible(false);

    hint_ = &col.add<Label>("");
    hint_->set_font_size(18);
    hint_->set_muted(true);
}

void PlaylistsPage::set_header(const std::string &crumb2, const std::string &crumb3, const std::string &count) {
    crumbs_->clear_children();
    auto crumb = [this](const std::string &text, bool last) {
        auto &c = crumbs_->add<Chip>(text, last ? ui::Color::hex(0x2A2A31FF) : col::surface, 21, last);
        c.set_color(last ? col::text : col::text_soft);
        c.set_align(Align::Start, Align::Center);
    };
    auto chevron = [this] {
        auto &c = crumbs_->add<ImageView>(ui::icon("chevron-right"));
        c.set_size(22, 22);
        c.set_fit(ImageFit::Contain);
        c.set_align(Align::Start, Align::Center);
    };
    crumb("Playlists", crumb2.empty());
    if (!crumb2.empty()) {
        chevron();
        crumb(crumb2, crumb3.empty());
    }
    if (!crumb3.empty()) {
        chevron();
        crumb(crumb3, true);
    }
    count_->set_text(count);
}

void PlaylistsPage::load_saved() {
    if (saved_loaded_) return;
    saved_loaded_ = true;
    wnk::storage::Entry e;
    auto *store = s_.ctx->store();
    if (store != nullptr && store->get(kSavedKey, &e) == wnk::storage::Status::Ok) saved_ = saved_playlists_from_json(e.value);
}

void PlaylistsPage::on_shown(Ui &ui) {
    // A list being opened (or shown) stays: showing the lists would cancel its loading.
    if (level_ != Level::Root || opening_) return;
    load_saved();
    show_root(ui, false);
    // Playlist files on the drives (a worker: drives can be slow).
    wnk::async::run_then_post<std::vector<platform::FileEntry>>(
        s_.ctx->pool(), s_.ctx->dispatcher(), lifetime_.weak(), wnk::async::CancellationToken{},
        [](const wnk::async::CancellationToken &) {
            return scan_files(media_volumes(), {platform::FileKind::Playlist}, 4, 20000);
        },
        [this](std::vector<platform::FileEntry> files) {
            files_ = std::move(files);
            // Not while a list opens: showing the lists would cancel its loading.
            if (level_ == Level::Root && !opening_) show_root(s_.ctx->ui(), true);
        });
}

void PlaylistsPage::show_root(Ui &ui, bool focus) {
    loading_.cancel();
    opening_ = false;
    level_ = Level::Root;
    const bool had = ui.has_focus_within(this);
    const size_t at = had ? grid_->focused_index() : 0;
    root_.clear();
    root_.push_back({RootItem::Kind::Add, "Add a playlist or IPTV list", ""});
    if (s_.my_playlists != nullptr)
        for (const auto &l : *s_.my_playlists) root_.push_back({RootItem::Kind::Mine, l.name, ""});
    for (const auto &l : saved_) root_.push_back({RootItem::Kind::Saved, l.name.empty() ? mrl_display_name(l.url) : l.name, l.url});
    for (const auto &f : files_) root_.push_back({RootItem::Kind::File, display_name(f.name), f.path});
    set_header("", "", std::to_string(root_.size() - 1) + (root_.size() == 2 ? " list" : " lists"));
    grid_->set_items(root_.size(), [this](Ui &, size_t i) -> std::unique_ptr<View> {
        const RootItem &r = root_[i];
        TileSpec t;
        t.title = r.name;
        t.icon_url = ui::icon(r.kind == RootItem::Kind::Add ? "link" : "playlist");
        if (r.kind == RootItem::Kind::Add) {
            t.subtitle = "M3U, M3U8, PLS or XSPF address";
            t.tint = ui::Color::hex(0x3A2410FF);
        } else if (r.kind == RootItem::Kind::Mine) {
            size_t n = 0;
            for (const auto &l : *s_.my_playlists)
                if (l.name == r.name) n = l.items.size();
            t.subtitle = "Your playlist · " + std::to_string(n) + (n == 1 ? " entry" : " entries");
            t.tint = ui::tint_for(r.name);
        } else if (r.kind == RootItem::Kind::Saved) {
            t.subtitle = mrl_protocol(r.location) + " · added by address";
        } else {
            t.subtitle = extension_upper(r.location) + " · on a drive";
            t.badge = extension_upper(r.location);
        }
        return std::make_unique<Tile>(t);
    });
    message_->set_visible(false);
    hint_->set_text("Red: forget a list added by address, or delete one of your playlists.");
    if ((focus && had) || focus) grid_->focus_index(ui, std::min(at, root_.size() - 1));
}

void PlaylistsPage::open_list(Ui &ui, const std::string &name, const std::string &location) {
    mine_ = -1;
    loading_.cancel();
    loading_ = wnk::async::CancellationSource();
    list_name_ = name;
    list_location_ = location;
    opening_ = true;
    WNK_LOGI(kCat, "open list %s", location.c_str());
    set_header(name, "", "Loading…");
    auto http = s_.ctx->http();
    wnk::async::run_then_post<Loaded>(
        s_.ctx->pool(), s_.ctx->dispatcher(), lifetime_.weak(), loading_.token(),
        [location, http](const wnk::async::CancellationToken &cancel) {
            Loaded r;
            std::string text;
            if (location.find("://") == std::string::npos) {
                if (!read_local(location, &text, &r.error)) return r;
            } else {
                if (!http) {
                    r.error = "this device cannot fetch addresses";
                    return r;
                }
                wnk::net::HttpRequest req;
                req.url = location;
                req.max_response_bytes = kMaxListBytes;
                req.timeout_ms = 30000;
                const auto t0 = std::chrono::steady_clock::now();
                const auto resp = http->perform(req, cancel);
                WNK_LOGI(kCat, "fetched %s: status %d, %zu bytes, %lld ms", location.c_str(), resp.status, resp.body.size(),
                         static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                    std::chrono::steady_clock::now() - t0).count()));
                if (!resp.ok()) {
                    r.error = resp.error != wnk::net::NetError::None ? std::string("network: ") + wnk::net::error_name(resp.error)
                                                                    : "the server answered " + std::to_string(resp.status);
                    return r;
                }
                text = resp.body;
            }
            if (!parse_playlist(text, location, &r.list, &r.error) && r.error.empty()) r.error = "unreadable";
            return r;
        },
        [this](Loaded r) { show_list(s_.ctx->ui(), std::move(r.list), r.error); });
    (void)ui;
}

void PlaylistsPage::show_list(Ui &ui, Playlist list, const std::string &error) {
    opening_ = false;
    if (!error.empty()) {
        WNK_LOGW(kCat, "%s: %s", list_location_.c_str(), error.c_str());
        ui.toast("Cannot open " + list_name_ + ": " + error, 5000);
        show_root(ui, true);
        return;
    }
    if (list.is_stream) {  // an HLS address: it plays
        s_.play_queue({to_file_entry({list_name_, list_location_, NetEntry::Kind::Media, {}})}, 0);
        show_root(ui, true);
        return;
    }
    list_ = std::move(list);
    groups_ = list_.groups();
    WNK_LOGI(kCat, "%s: %zu entries in %zu groups", list_location_.c_str(), list_.items.size(), groups_.size());
    if (list_.items.empty()) {
        ui.toast(list_name_ + " is empty.");
        show_root(ui, true);
        return;
    }
    if (groups_.size() > 1) {
        show_groups(ui, 0);
    } else {
        show_entries(ui, groups_.empty() ? std::string() : groups_[0]);
    }
}

void PlaylistsPage::show_groups(Ui &ui, size_t focus) {
    level_ = Level::Groups;
    set_header(list_name_, "", std::to_string(groups_.size()) + " groups · " + std::to_string(list_.items.size()) + " entries");
    grid_->set_items(groups_.size(), [this](Ui &, size_t i) -> std::unique_ptr<View> {
        const std::string &g = groups_[i];
        const size_t n = static_cast<size_t>(std::count_if(list_.items.begin(), list_.items.end(),
                                                           [&](const PlaylistItem &x) { return x.group == g; }));
        TileSpec t;
        t.title = g.empty() ? "Other" : g;
        t.subtitle = std::to_string(n) + (n == 1 ? " entry" : " entries");
        t.icon_url = ui::icon("folder");
        return std::make_unique<Tile>(t);
    });
    hint_->set_text("Back: the lists");
    grid_->focus_index(ui, std::min(focus, groups_.size() - 1));
}

void PlaylistsPage::show_entries(Ui &ui, const std::string &group) {
    level_ = Level::Entries;
    group_ = group;
    shown_.clear();
    for (const auto &i : list_.items)
        if (i.group == group || groups_.size() <= 1) shown_.push_back(&i);
    set_header(list_name_, groups_.size() > 1 ? (group.empty() ? "Other" : group) : "",
               std::to_string(shown_.size()) + (shown_.size() == 1 ? " entry" : " entries"));
    grid_->set_items(shown_.size(), [this](Ui &, size_t i) -> std::unique_ptr<View> {
        const PlaylistItem &it = *shown_[i];
        TileSpec t;
        t.title = it.title;
        const platform::FileKind k = platform::file_kind(it.location);
        if (!it.logo.empty()) t.logo_url = it.logo; else t.icon_url = ui::icon(k == platform::FileKind::Audio ? "music" : "film");
        t.tint = ui::tint_for(it.title);
        const std::string d = duration_text(it.duration_ms);
        t.subtitle = it.location.find("://") != std::string::npos ? mrl_protocol(it.location) : "File";
        if (!d.empty()) t.corner = d;
        return std::make_unique<Tile>(t);
    });
    hint_->set_text(groups_.size() > 1 ? "Back: the groups" : "Back: the lists");
    grid_->focus_index(ui, 0);
}

void PlaylistsPage::activate(Ui &ui, size_t i) {
    switch (level_) {
        case Level::Root: {
            if (i >= root_.size()) return;
            const RootItem r = root_[i];
            if (r.kind == RootItem::Kind::Add) {
                open_address_dialog(ui);
            } else if (r.kind == RootItem::Kind::Mine) {
                for (size_t k = 0; k < s_.my_playlists->size(); ++k) {
                    if ((*s_.my_playlists)[k].name != r.name) continue;
                    mine_ = static_cast<int>(k);
                    list_name_ = r.name;
                    list_location_.clear();
                    Playlist pl;
                    pl.items = (*s_.my_playlists)[k].items;
                    show_list(ui, std::move(pl), "");
                }
            } else {
                open_list(ui, r.name, r.location);
            }
            return;
        }
        case Level::Groups:
            if (i < groups_.size()) show_entries(ui, groups_[i]);
            return;
        case Level::Entries: {
            if (i >= shown_.size()) return;
            std::vector<platform::FileEntry> queue;
            for (const auto *it : shown_) {
                platform::FileEntry e;
                e.name = it->title;
                e.path = it->location;
                e.kind = platform::file_kind(it->location) == platform::FileKind::Audio ? platform::FileKind::Audio
                                                                                         : platform::FileKind::Video;
                queue.push_back(std::move(e));
            }
            s_.play_queue(std::move(queue), i);
            return;
        }
    }
}

bool PlaylistsPage::on_action(Ui &ui, const input::ActionEvent &e) {
    // Red inside one of the viewer's own playlists removes the entry.
    if (e.action == input::Action::Red && level_ == Level::Entries && mine_ >= 0 &&
        mine_ < static_cast<int>(s_.my_playlists->size())) {
        if (e.phase != input::Phase::Press) return true;
        const size_t i = grid_->focused_index();
        if (i < shown_.size()) {
            const std::string loc = shown_[i]->location, title = shown_[i]->title;
            auto &items = (*s_.my_playlists)[static_cast<size_t>(mine_)].items;
            items.erase(std::remove_if(items.begin(), items.end(), [&](const PlaylistItem &x) { return x.location == loc; }),
                        items.end());
            if (s_.save_my_playlists) s_.save_my_playlists();
            ui.toast("Removed " + title);
            list_.items = items;
            groups_ = list_.groups();
            if (list_.items.empty()) {
                show_root(ui, true);
            } else {
                show_entries(ui, groups_.empty() ? std::string() : groups_[0]);
            }
        }
        return true;
    }
    if (e.action == input::Action::Red && level_ == Level::Root) {
        if (e.phase != input::Phase::Press) return true;
        const size_t i = grid_->focused_index();
        if (i < root_.size() && root_[i].kind == RootItem::Kind::Mine) {
            const std::string name = root_[i].name;
            auto &mine = *s_.my_playlists;
            mine.erase(std::remove_if(mine.begin(), mine.end(), [&](const MyPlaylist &l) { return l.name == name; }), mine.end());
            if (s_.save_my_playlists) s_.save_my_playlists();
            ui.toast("Deleted " + name);
            show_root(ui, true);
        } else if (i < root_.size() && root_[i].kind == RootItem::Kind::Saved) {
            const std::string url = root_[i].location, name = root_[i].name;
            saved_.erase(std::remove_if(saved_.begin(), saved_.end(), [&](const SavedPlaylist &l) { return l.url == url; }),
                         saved_.end());
            save_lists();
            ui.toast("Forgot " + name);
            show_root(ui, true);
        }
        return true;
    }
    if (e.action != input::Action::Back || level_ == Level::Root) return Page::on_action(ui, e);
    if (e.phase != input::Phase::Press) return true;
    if (level_ == Level::Entries && groups_.size() > 1) {
        const auto it = std::find(groups_.begin(), groups_.end(), group_);
        show_groups(ui, it == groups_.end() ? 0 : static_cast<size_t>(it - groups_.begin()));
    } else {
        show_root(ui, true);
    }
    return true;
}

void PlaylistsPage::save_lists() {
    auto *store = s_.ctx->store();
    if (store == nullptr) return;
    s_.ctx->pool().submit([store, json = saved_playlists_json(saved_)](const wnk::async::CancellationToken &) {
        if (store->put(kSavedKey, json).status != wnk::storage::Status::Ok) WNK_LOGW(kCat, "lists not stored");
    });
}

void PlaylistsPage::add_address(Ui &ui, const std::string &label, const std::string &address) {
    load_saved();  // before adding to them (they load when the page is first shown)
    const std::string name = label.empty() ? mrl_display_name(address) : label;
    saved_.erase(std::remove_if(saved_.begin(), saved_.end(), [&](const SavedPlaylist &l) { return l.url == address; }),
                 saved_.end());
    saved_.push_back({name, address});
    save_lists();
    show_root(ui, true);
    open_list(ui, name, address);
}

void PlaylistsPage::open_address_dialog(Ui &ui) {
    auto d = std::make_unique<Dialog>("Add a playlist or IPTV list",
                                      "The list's address (M3U, M3U8, PLS or XSPF). It stays here for next time.");
    auto &url = d->body().add<TextField>("http://provider.example/playlist.m3u");
    auto &label = d->body().add<TextField>("Name (optional): My TV channels");
    d->add_button("Add", [this, &url, &label](Ui &u) {
        const std::string address = url.text();
        if (!looks_like_url(address)) {
            u.toast("That does not look like an address (it needs a scheme, like http://).");
            return;
        }
        add_address(u, label.text(), address);
    });
    d->add_button("Cancel");
    ui.show_dialog(std::move(d));
    ui.focus(&url);
}

}  // namespace vplayer
