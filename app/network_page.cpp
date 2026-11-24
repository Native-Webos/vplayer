// SPDX-License-Identifier: GPL-3.0-or-later
#include "network_page.hpp"

#include "vlc_engine.hpp"

#include "library.hpp"

#include "wnk/log.hpp"
#include "wnk/ui/forms.hpp"
#include "wnk/ui/navigation.hpp"

#include <algorithm>

namespace vplayer {

using namespace wnk::ui;
using wnk::Size;
namespace input = wnk::input;
using vplayer::ui::Chip;
using vplayer::ui::Tile;
using vplayer::ui::TileSpec;
namespace col = vplayer::ui::col;
namespace metric = vplayer::ui::metric;

namespace {

constexpr const char *kCat = "network";
constexpr float kPollSeconds = 2;

const std::vector<std::string> kProtocols = {"SMB (Windows, NAS)", "NFS", "FTP", "SFTP", "WebDAV / HTTP"};
const std::vector<std::string> kSchemes = {"smb", "nfs", "ftp", "sftp", "http"};

TileSpec entry_tile(const NetEntry &e) {
    TileSpec t;
    t.title = e.kind == NetEntry::Kind::Folder || e.kind == NetEntry::Kind::Server ? e.name : display_name(e.name);
    switch (e.kind) {
        case NetEntry::Kind::Server:
            t.icon_url = ui::icon("server");
            t.subtitle = e.detail;
            break;
        case NetEntry::Kind::Folder:
            t.icon_url = ui::icon("folder");
            t.subtitle = "Folder";
            break;
        case NetEntry::Kind::Video:
        case NetEntry::Kind::Media:
            t.icon_url = ui::icon("film");
            t.tint = ui::tint_for(e.name);
            t.badge = extension_upper(e.name);
            t.subtitle = e.kind == NetEntry::Kind::Media ? "Media" : "Video";
            break;
        case NetEntry::Kind::Audio:
            t.icon_url = ui::icon("music");
            t.tint = ui::tint_for(e.name);
            t.badge = extension_upper(e.name);
            t.subtitle = "Audio";
            break;
        case NetEntry::Kind::Subtitle:
            t.icon_url = ui::icon("subtitle");
            t.subtitle = "Subtitles · " + extension_upper(e.name);
            break;
        default:
            t.icon_url = ui::icon("file");
            t.subtitle = "File";
            break;
    }
    return t;
}

bool same_servers(const std::vector<NetEntry> &a, const std::vector<NetEntry> &b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (a[i].mrl != b[i].mrl || a[i].name != b[i].name) return false;
    return true;
}

}  // namespace

NetworkPage::NetworkPage(Services &services, libvlc_instance_t *vlc)
    : s_(services), browser_(make_network_browser(vlc)) {
    name = "page.network";
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
    grid_ = &body.add<ui::MenuGrid>(Axis::Vertical, 5, Size{metric::tile_w, metric::tile_h}, metric::tile_gap);
    grid_->has_menu = [this](size_t i) {
        return folder_.empty() && i < root_.size() && (root_[i].what == Item::Saved || root_[i].what == Item::Recent);
    };
    grid_->on_menu = [this](Ui &u, size_t i) { item_menu(u, i); };
    grid_->set_content_padding(Insets{12, 20, 12, 12});
    grid_->name = "network.grid";
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

void NetworkPage::set_vlc(libvlc_instance_t *vlc) {
    browser_ = make_network_browser(vlc);
    found_.clear();
    if (folder_.empty() && places_loaded_) show_root(s_.ctx->ui(), true);
}

void NetworkPage::on_shown(Ui &ui) {
    if (!places_loaded_) {
        places_loaded_ = true;
        places_ = load_saved_places(s_.ctx->store());
    }
    if (folder_.empty()) show_root(ui, false);
}

void NetworkPage::update(Ui &ui, float dt) {
    Page::update(ui, dt);
    poll_ += dt;
    if (poll_ < kPollSeconds || !folder_.empty() || !places_loaded_) return;
    poll_ = 0;
    auto found = browser_->servers();
    if (same_servers(found, found_)) return;
    found_ = std::move(found);
    show_root(ui, true);
}

void NetworkPage::set_crumbs() {
    crumbs_->clear_children();
    auto chevron = [this] {
        auto &c = crumbs_->add<ImageView>(ui::icon("chevron-right"));
        c.set_size(22, 22);
        c.set_fit(ImageFit::Contain);
        c.set_align(Align::Start, Align::Center);
    };
    auto crumb = [this](const std::string &text, bool last) {
        auto &c = crumbs_->add<Chip>(text, last ? ui::Color::hex(0x2A2A31FF) : col::surface, 21, last);
        c.set_color(last ? col::text : col::text_soft);
        c.set_align(Align::Start, Align::Center);
    };
    crumb("Network", folder_.empty());
    if (folder_.empty()) return;
    std::vector<std::string> parts{server_.name};
    // The folders below the server's address.
    std::string rel = folder_.size() > server_.mrl.size() && folder_.compare(0, server_.mrl.size(), server_.mrl) == 0
                          ? folder_.substr(server_.mrl.size())
                          : std::string();
    size_t start = 0;
    while (start < rel.size()) {
        size_t slash = rel.find('/', start);
        if (slash == std::string::npos) slash = rel.size();
        if (slash > start) parts.push_back(mrl_display_name(rel.substr(start, slash - start)));
        start = slash + 1;
    }
    if (parts.size() > 3) parts.erase(parts.begin() + 1, parts.end() - 2), parts.insert(parts.begin() + 1, "…");
    for (size_t i = 0; i < parts.size(); ++i) {
        chevron();
        crumb(parts[i], i + 1 == parts.size());
    }
}

void NetworkPage::show_root(Ui &ui, bool keep_focus) {
    listing_.cancel();
    const bool had_focus = ui.has_focus_within(this);
    const size_t focused = had_focus ? grid_->focused_index() : 0;
    folder_.clear();
    entries_.clear();
    root_.clear();
    root_.push_back({Item::OpenUrl, {}});
    root_.push_back({Item::AddServer, {}});
    for (const auto &s : places_.servers) root_.push_back({Item::Saved, s});
    for (const auto &s : found_) {
        const bool saved = std::any_of(places_.servers.begin(), places_.servers.end(),
                                       [&](const NetEntry &x) { return x.mrl == s.mrl; });
        if (!saved) root_.push_back({Item::Found, s});
    }
    for (const auto &u : places_.urls) root_.push_back({Item::Recent, {u, u, NetEntry::Kind::Media, "Recently opened"}});
    set_crumbs();
    const size_t servers = places_.servers.size() + found_.size();
    count_->set_text(servers == 1 ? "1 server" : std::to_string(servers) + " servers");
    grid_->set_items(root_.size(), [this](Ui &, size_t i) -> std::unique_ptr<View> {
        const RootItem &r = root_[i];
        TileSpec t;
        switch (r.what) {
            case Item::OpenUrl:
                t.title = "Open a network stream";
                t.subtitle = "An address: https://, rtsp://, smb://…";
                t.icon_url = ui::icon("link");
                t.tint = ui::Color::hex(0x3A2410FF);  // a warm tint of the accent
                break;
            case Item::AddServer:
                t.title = "Add a server";
                t.subtitle = "SMB, NFS, FTP, SFTP or WebDAV by address";
                t.icon_url = ui::icon("server");
                break;
            case Item::Saved:
            case Item::Found: t = entry_tile(r.entry); break;
            case Item::Recent:
                t.title = mrl_display_name(r.entry.mrl);
                t.subtitle = mrl_protocol(r.entry.mrl) + " · recently opened";
                t.icon_url = ui::icon("link");
                break;
        }
        if (r.what == Item::Saved || r.what == Item::Recent) t.on_menu = [this, i](Ui &u) { item_menu(u, i); };
        return std::make_unique<Tile>(t);
    });
    message_->set_visible(false);
    const char *remove = places_.servers.empty() && places_.urls.empty()
                             ? ""
                             : "Hold OK on a saved server for its options (edit, remove), or click its three dots.";
    hint_->set_text(found_.empty() ? std::string("Looking for media servers on this network… ") + remove : std::string(remove));
    if ((keep_focus && had_focus) || want_focus_) {
        want_focus_ = false;
        grid_->focus_index(ui, std::min(focused, root_.size() - 1));
    }
}

void NetworkPage::open(Ui &ui, const NetEntry &server, const std::string &mrl, const std::string &focus_mrl) {
    listing_.cancel();
    listing_ = wnk::async::CancellationSource();
    server_ = server;
    folder_ = mrl;
    want_focus_ = true;
    set_crumbs();
    count_->set_text("Loading…");
    hint_->set_text("Back: up a folder");
    // The tiles stay until the listing arrives: clearing them would drop focus onto the tabs, and
    // focusing a tab switches pages.
    NetworkBrowser *b = browser_.get();
    wnk::async::run_then_post<std::pair<std::vector<NetEntry>, std::string>>(
        s_.ctx->pool(), s_.ctx->dispatcher(), lifetime_.weak(), listing_.token(),
        [b, mrl](const wnk::async::CancellationToken &cancel) {
            std::pair<std::vector<NetEntry>, std::string> r;
            if (!b->list(mrl, &r.first, &r.second, cancel) && r.second.empty()) r.second = "unknown error";
            return r;
        },
        [this, focus_mrl](std::pair<std::vector<NetEntry>, std::string> r) {
            show_entries(s_.ctx->ui(), std::move(r.first), r.second, focus_mrl);
        });
    (void)ui;
}

void NetworkPage::show_entries(Ui &ui, std::vector<NetEntry> entries, const std::string &error,
                               const std::string &focus_mrl) {
    if (folder_.empty()) return;  // went back meanwhile
    entries_ = std::move(entries);
    const size_t n = entries_.size();
    count_->set_text(n == 1 ? "1 item" : std::to_string(n) + " items");
    grid_->set_items(n, [this](Ui &, size_t i) -> std::unique_ptr<View> { return std::make_unique<Tile>(entry_tile(entries_[i])); });
    message_->set_text(!error.empty() ? "This folder cannot be opened.\n" + error : "This folder is empty.");
    message_->set_visible(n == 0);
    if (n > 0 && (want_focus_ || ui.has_focus_within(this))) {
        want_focus_ = false;
        size_t index = 0;
        for (size_t i = 0; i < n; ++i)
            if (entries_[i].mrl == focus_mrl) index = i;
        grid_->focus_index(ui, index);
    }
    WNK_LOGI(kCat, "browse %s: %zu entries%s%s", folder_.c_str(), n, error.empty() ? "" : ", error: ", error.c_str());
}

void NetworkPage::activate(Ui &ui, size_t i) {
    if (folder_.empty()) {
        if (i >= root_.size()) return;
        const RootItem r = root_[i];
        switch (r.what) {
            case Item::OpenUrl: ask_url(ui); break;
            case Item::AddServer: ask_server(ui); break;
            case Item::Saved:
            case Item::Found: open(ui, r.entry, r.entry.mrl); break;
            case Item::Recent: play_url(ui, r.entry.mrl); break;
        }
        return;
    }
    if (i >= entries_.size()) return;
    const NetEntry e = entries_[i];
    if (e.kind == NetEntry::Kind::Folder || e.kind == NetEntry::Kind::Server) {
        open(ui, server_, e.mrl);
        return;
    }
    if (e.kind == NetEntry::Kind::Subtitle) {
        ui.toast("Subtitle files are used automatically when the video next to them plays.");
        return;
    }
    if (!e.playable()) {
        ui.toast("VPlayer cannot open this kind of file.");
        return;
    }
    // The folder's playable entries are the queue (previous / next / play the next one).
    std::vector<wnk::platform::FileEntry> queue;
    size_t index = 0;
    for (const auto &x : entries_) {
        if (!x.playable()) continue;
        if (x.mrl == e.mrl) index = queue.size();
        queue.push_back(to_file_entry(x));
    }
    s_.play_queue(std::move(queue), index);
}

bool NetworkPage::on_action(Ui &ui, const input::ActionEvent &e) {
    if (e.action == input::Action::Red && folder_.empty()) {
        if (e.phase == input::Phase::Press) forget(ui, grid_->focused_index());
        return true;
    }
    if (e.action != input::Action::Back || folder_.empty()) return Page::on_action(ui, e);
    if (e.phase != input::Phase::Press) return true;
    const std::string parent = mrl_parent(folder_);
    // Up a folder while inside the server, else back to the servers.
    if (parent.empty() || folder_ == server_.mrl || parent.size() < server_.mrl.size()) {
        want_focus_ = true;
        show_root(ui, false);
    } else {
        open(ui, server_, parent, folder_);
    }
    return true;
}

void NetworkPage::forget(Ui &ui, size_t i) {
    if (i >= root_.size()) return;
    const RootItem r = root_[i];
    if (r.what == Item::Saved || r.what == Item::Recent) confirm_remove(ui, r.what == Item::Saved, r.entry);
}

void NetworkPage::item_menu(Ui &ui, size_t i) {
    if (!folder_.empty() || i >= root_.size()) return;
    const RootItem r = root_[i];
    const bool server = r.what == Item::Saved;
    if (!server && r.what != Item::Recent) return;
    auto d = std::make_unique<Dialog>(server ? r.entry.name : mrl_display_name(r.entry.mrl), loggable(r.entry.mrl));
    if (server) d->add_button("Edit", [this, e = r.entry](Ui &u) { edit_server(u, e); });
    d->add_button("Remove", [this, server, e = r.entry](Ui &u) { confirm_remove(u, server, e); });
    d->add_button("Cancel");
    ui.show_dialog(std::move(d));
}

void NetworkPage::edit_server(Ui &ui, const NetEntry &entry) {
    auto d = std::make_unique<Dialog>("Edit " + entry.name, "Its name, and its address (smb://, nfs://, ftp://, sftp://...).");
    auto &label = d->body().add<TextField>("Name");
    label.set_text(entry.name);
    auto &address = d->body().add<TextField>("Address");
    address.set_text(entry.mrl);
    d->add_button("Save", [this, old = entry.mrl, &label, &address](Ui &u) {
        std::string mrl = address.text();
        while (!mrl.empty() && mrl.back() == ' ') mrl.pop_back();
        if (!looks_like_url(mrl)) {
            u.toast("That does not look like an address (it needs a scheme, like smb://).");
            return;
        }
        if (mrl.back() != '/') mrl += '/';
        for (auto &s : places_.servers) {
            if (s.mrl != old) continue;
            s.mrl = mrl;
            s.name = label.text().empty() ? mrl_display_name(mrl) : label.text();
            s.detail = mrl_protocol(mrl) + " · saved";
        }
        save_places();
        WNK_LOGI(kCat, "edited server %s", loggable(mrl).c_str());
        u.toast("Saved");
        want_focus_ = true;
        show_root(u, true);
    });
    d->add_button("Cancel");
    ui.show_dialog(std::move(d));
}

void NetworkPage::confirm_remove(Ui &ui, bool server, const NetEntry &entry) {
    const std::string what = server ? "the server " : "the address ";
    auto d = std::make_unique<Dialog>("Remove " + entry.name + "?",
                                      "Removes " + what + loggable(entry.mrl) + " from this list. Its files are not touched.");
    d->add_button("Remove", [this, server, mrl = entry.mrl, name = entry.name](Ui &u) { remove_place(u, server, mrl, name); });
    d->add_button("Cancel");
    ui.show_dialog(std::move(d));
}

void NetworkPage::remove_place(Ui &ui, bool server, const std::string &mrl, const std::string &name) {
    if (server) {
        places_.servers.erase(std::remove_if(places_.servers.begin(), places_.servers.end(),
                                             [&](const NetEntry &x) { return x.mrl == mrl; }),
                              places_.servers.end());
    } else {
        places_.urls.erase(std::remove(places_.urls.begin(), places_.urls.end(), mrl), places_.urls.end());
    }
    save_places();
    WNK_LOGI(kCat, "removed %s %s", server ? "server" : "address", loggable(mrl).c_str());
    ui.toast("Removed " + name);
    want_focus_ = true;
    show_root(ui, true);
}

void NetworkPage::save_places() {
    auto *store = s_.ctx->store();
    const SavedPlaces copy = places_;
    s_.ctx->pool().submit([store, copy](const wnk::async::CancellationToken &) {
        if (!save_saved_places(store, copy)) WNK_LOGW(kCat, "saved servers not stored");
    });
}

void NetworkPage::play_url(Ui &ui, const std::string &url) {
    NetEntry e{mrl_display_name(url), url, NetEntry::Kind::Media, {}};
    if (e.name.empty()) e.name = url;
    remember_url(&places_, url);
    save_places();
    if (folder_.empty()) show_root(ui, true);
    s_.play_queue({to_file_entry(e)}, 0);
}

void NetworkPage::ask_url(Ui &ui) {
    auto d = std::make_unique<Dialog>("Open a network stream",
                                      "A stream or file address: https://, rtsp://, rtmp://, udp://, smb://, ftp://, a playlist (.m3u8)…");
    auto &field = d->body().add<TextField>("https://example.com/stream.m3u8");
    field.set_on_submit([this](Ui &u, const std::string &text) {
        if (looks_like_url(text)) {
            u.dismiss_dialog(u.top_dialog());
            play_url(u, text);
        }
    });
    d->add_button("Play", [this, &field](Ui &u) {
        const std::string text = field.text();
        if (!looks_like_url(text)) {
            u.toast("That does not look like an address (it needs a scheme, like https://).");
            return;
        }
        play_url(u, text);
    });
    d->add_button("Cancel");
    ui.show_dialog(std::move(d));
    ui.focus(&field);
}

void NetworkPage::ask_server(Ui &ui) {
    auto d = std::make_unique<Dialog>("Add a server", "The server's name or IP address, and optionally a share or folder. "
                                                      "VPlayer asks for a user name and password if the server needs one.");
    auto protocol = std::make_shared<int>(0);
    d->body().add<Choice>("Protocol", kProtocols, 0, [protocol](Ui &, int i) { *protocol = i; });
    auto &host = d->body().add<TextField>("Address: nas.local or 192.168.1.20");
    auto &path = d->body().add<TextField>("Share or folder (optional): films");
    auto &label = d->body().add<TextField>("Name (optional): Living room NAS");
    d->add_button("Add", [this, protocol, &host, &path, &label](Ui &u) {
        std::string h = host.text();
        while (!h.empty() && (h.back() == '/' || h.back() == ' ')) h.pop_back();
        while (!h.empty() && h.front() == ' ') h.erase(h.begin());
        if (h.empty()) {
            u.toast("The server needs an address.");
            return;
        }
        std::string p = path.text();
        while (!p.empty() && (p.front() == '/' || p.front() == ' ')) p.erase(p.begin());
        while (!p.empty() && (p.back() == '/' || p.back() == ' ')) p.pop_back();
        NetEntry e;
        e.mrl = kSchemes[static_cast<size_t>(*protocol)] + "://" + h + "/" + (p.empty() ? "" : p + "/");
        e.name = label.text().empty() ? (p.empty() ? h : h + " · " + p) : label.text();
        e.kind = NetEntry::Kind::Server;
        e.detail = mrl_protocol(e.mrl) + " · saved";
        places_.servers.erase(std::remove_if(places_.servers.begin(), places_.servers.end(),
                                             [&](const NetEntry &x) { return x.mrl == e.mrl; }),
                              places_.servers.end());
        places_.servers.push_back(e);
        save_places();
        show_root(u, true);
        open(u, e, e.mrl);
    });
    d->add_button("Cancel");
    ui.show_dialog(std::move(d));
}

}  // namespace vplayer
