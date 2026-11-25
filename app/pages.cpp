// SPDX-License-Identifier: GPL-3.0-or-later
#include "pages.hpp"

#include <sys/stat.h>

#include "library.hpp"
#include "thumbnails.hpp"

#include "wnk/log.hpp"

#include <algorithm>
#include <utility>

namespace vplayer {

using namespace wnk::ui;
using wnk::Size;
namespace platform = wnk::platform;
namespace input = wnk::input;
using vplayer::ui::Chip;
using vplayer::ui::HeroCard;
using vplayer::ui::Tile;
using vplayer::ui::TileSpec;
namespace col = vplayer::ui::col;
namespace metric = vplayer::ui::metric;

namespace {

constexpr const char *kCat = "vplayer";
constexpr float kPollSeconds = 2;

// LG mounts drives with its own drivers ("tfat", "texfat", "tntfs"); people know them without the t.
std::string fs_label(const std::string &fs) {
    std::string s = fs;
    if (s == "tfat" || s == "texfat" || s == "tntfs") s = s.substr(1);
    for (char &c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

std::string volume_detail(const platform::RemovableVolume &v) {
    std::string d;
    if (v.total_bytes > 0) d = format_bytes(v.free_bytes) + " free of " + format_bytes(v.total_bytes);
    const std::string fs = fs_label(v.fs_type);
    if (!fs.empty() && fs != "FOLDER") d += (d.empty() ? "" : " · ") + fs;
    return d.empty() ? "Removable drive" : d;
}

std::string base_name(const std::string &path) {
    const size_t slash = path.rfind('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string parent_of(const std::string &path) {
    const size_t slash = path.rfind('/');
    return slash == std::string::npos || slash == 0 ? std::string("/") : path.substr(0, slash);
}

TileSpec entry_tile(const platform::FileEntry &e, Thumbnails *thumbs = nullptr) {
    TileSpec t;
    if (const std::string kind = disc_folder_kind(e.path); !kind.empty()) {  // a disc folder (first)
        t.title = e.name;
        t.icon_url = ui::icon("disc");
        t.tint = ui::tint_for(e.name);
        t.badge = kind;
        t.subtitle = kind == "DVD" ? "Play this DVD folder" : "Play this Blu-ray folder";
        return t;
    }
    if (is_disc(e)) {
        t.title = display_name(e.name);
        t.icon_url = ui::icon("disc");
        t.tint = ui::tint_for(e.name);
        t.badge = extension_upper(e.name);
        t.subtitle = "Disc image · " + format_bytes(e.size);
        return t;
    }
    t.title = e.kind == platform::FileKind::Folder ? e.name : display_name(e.name);
    const std::string size = format_bytes(e.size);
    switch (e.kind) {
        case platform::FileKind::Folder:
            t.icon_url = ui::icon("folder");
            t.subtitle = "Folder";
            break;
        case platform::FileKind::Video:
            if (thumbs != nullptr) t.image_url = thumbs->get(e);
            t.icon_url = ui::icon("film");
            t.tint = ui::tint_for(e.name);
            t.badge = extension_upper(e.name);
            t.subtitle = "Video · " + size;
            break;
        case platform::FileKind::Audio:
            t.icon_url = ui::icon("music");
            t.tint = ui::tint_for(e.name);
            t.badge = extension_upper(e.name);
            t.subtitle = "Audio · " + size;
            break;
        case platform::FileKind::Subtitle:
            t.icon_url = ui::icon("subtitle");
            t.subtitle = "Subtitles · " + extension_upper(e.name);
            break;
        case platform::FileKind::Playlist:
            t.icon_url = ui::icon("playlist");
            t.subtitle = "Playlist · " + extension_upper(e.name);
            break;
        case platform::FileKind::Image:
            t.icon_url = ui::icon("image");
            t.subtitle = "Picture · " + size;
            break;
        default:
            t.icon_url = ui::icon("file");
            t.subtitle = size;
            break;
    }
    return t;
}

Label &section_title(View &parent, const std::string &text) {
    auto &l = parent.add<Label>(text);
    l.set_font_size(30);
    l.set_bold(true);
    return l;
}

}  // namespace

// ==== HomePage =======================================================================================

HomePage::HomePage(Services &services) : s_(services) {
    name = "page.video";
    auto &col = add<Column>(36);

    auto &heroes = col.add<Row>(32);
    heroes.set_height(Dim::fixed(400));
    hero_ = &heroes.add<HeroCard>(ui::icon("usb"), "GET STARTED", "Plug in a USB drive",
                                  "Or open a share on your network.", ui::tint_for("hero"),
                                  [this](Ui &) {
                                      if (!volumes_.empty()) {
                                          s_.browse_to(volumes_.front().path);
                                      } else {
                                          s_.show_tab(Tab::Browse);
                                      }
                                  });
    hero_->set_width(Dim::fixed(920));
    hero_->name = "home.hero";

    auto &side = heroes.add<Column>(32);
    side.set_width(Dim::fill());
    auto &net = side.add<HeroCard>(ui::icon("server"), "NETWORK", "Network shares", "SMB, NFS, SFTP, FTP and DLNA",
                                   ui::Color::hex(0x3B2F45FF), [this](Ui &) { s_.show_tab(Tab::Network); }, 30);
    net.set_height(Dim::fill());
    net.name = "home.network";
    auto &stream = side.add<HeroCard>(ui::icon("link"), "STREAM", "Open a stream", "HTTP, HLS, RTSP and IPTV lists",
                                      ui::Color::hex(0x2F4538FF),
                                      [this](Ui &) { s_.open_stream(); }, 30);
    stream.set_height(Dim::fill());
    stream.name = "home.stream";

    auto &header = col.add<Row>(16);
    header.set_align(Align::Stretch, Align::Start);
    rail_tabs_ = &header.add<ui::PillTabs>(std::vector<std::string>{"Continue watching", "Recently added"},
                                           [this](Ui &u, int i) {
                                               rail_mode_ = i;
                                               show_rail(u);
                                           });
    rail_tabs_->set_selected(1);
    rail_tabs_->tab(0).set_visible(false);  // until something can be continued
    rail_tabs_->set_align(Align::Start, Align::End);
    recent_source_ = &header.add<Label>("");
    recent_source_->set_font_size(20);
    recent_source_->set_muted(true);
    recent_source_->set_align(Align::Start, Align::End);

    auto &rail_box = col.add<View>();
    rail_box.set_height(Dim::fixed(metric::tile_h + 34));
    rail_ = &rail_box.add<CollectionView>(Axis::Horizontal, 1, Size{metric::tile_w, metric::tile_h}, metric::tile_gap);
    rail_->set_content_padding(Insets{12, 20, 12, 12});
    rail_->name = "home.recent";
    rail_->on_activate = [this](Ui &, size_t i) {
        if (rail_mode_ == 0) {
            if (i >= continue_.size()) return;
            platform::FileEntry e;
            e.name = continue_[i].name;
            e.path = continue_[i].path;
            e.kind = platform::file_kind(e.name) == platform::FileKind::Audio ? platform::FileKind::Audio
                                                                              : platform::FileKind::Video;
            s_.play(e);
            return;
        }
        if (i < recent_.size()) s_.play(recent_[i]);
    };
    recent_empty_ = &rail_box.add<Label>("");
    recent_empty_->set_font_size(22);
    recent_empty_->set_muted(true);
    recent_empty_->set_align(Align::Start, Align::Center);
    recent_empty_->set_padding(Insets{12, 0, 0, 0});
}

void HomePage::update(Ui &ui, float dt) {
    Page::update(ui, dt);
    if (s_.thumbnails != nullptr && s_.thumbnails->generation() != thumbs_gen_) {  // new thumbnails
        thumbs_gen_ = s_.thumbnails->generation();
        if ((rail_mode_ == 1 && !recent_.empty()) || (rail_mode_ == 0 && !continue_.empty())) {
            const bool had = ui.has_focus_within(rail_);
            const size_t at = rail_->focused_index();
            show_rail(ui);
            if (had) rail_->focus_index(ui, at);
        }
    }
    // Resume points changed (a file was played): the "Continue watching" list follows.
    if (s_.history != nullptr && s_.history->loaded() && s_.history->generation() != history_gen_) {
        history_gen_ = s_.history->generation();
        continue_ = s_.history->continue_list(20);
        rail_tabs_->tab(0).set_visible(!continue_.empty());
        if (!continue_.empty() && !continue_shown_) {  // once: do not move the rail under the viewer later
            continue_shown_ = true;
            rail_mode_ = 0;
            rail_tabs_->set_selected(0);
        }
        if (continue_.empty() && rail_mode_ == 0) {
            rail_mode_ = 1;
            rail_tabs_->set_selected(1);
        }
        show_rail(ui);
    }
    poll_ += dt;
    if (first_ || poll_ >= kPollSeconds) {
        refresh_volumes(ui, first_);
        first_ = false;
        poll_ = 0;
    }
}

void HomePage::refresh_volumes(Ui &ui, bool force) {
    auto vols = media_volumes();
    if (!force && same_volumes(vols, volumes_)) return;
    volumes_ = std::move(vols);

    if (volumes_.empty()) {
        hero_->set_texts("GET STARTED", "Plug in a USB drive",
                         "Or open a share on your network. Videos, music and playlists in almost any format.");
    } else {
        const auto &v = volumes_.front();
        std::string detail = volume_detail(v);
        if (volumes_.size() > 1) detail += " · " + std::to_string(volumes_.size() - 1) + " more";
        hero_->set_texts(v.fs_type == "folder" ? "FOLDER" : "USB DRIVE", v.name, detail);
    }
    recent_empty_->set_text(volumes_.empty() ? "Plug in a USB drive to see its newest videos here."
                                             : "Looking for videos…");
    recent_empty_->set_visible(true);
    (void)ui;

    auto vols_copy = volumes_;
    wnk::async::run_then_post<std::vector<platform::FileEntry>>(
        s_.ctx->pool(), s_.ctx->dispatcher(), lifetime_.weak(), wnk::async::CancellationToken{},
        [vols_copy](const wnk::async::CancellationToken &) { return recent_videos(vols_copy, 20); },
        [this](std::vector<platform::FileEntry> files) { show_recent(s_.ctx->ui(), std::move(files)); });
}

void HomePage::show_recent(Ui &ui, std::vector<platform::FileEntry> files) {
    recent_ = std::move(files);
    recent_loaded_ = true;
    show_rail(ui);
    WNK_LOGI(kCat, "home ready: %zu drives, %zu recent videos", volumes_.size(), recent_.size());
}

void HomePage::show_rail(Ui &) {
    if (rail_mode_ == 0) {
        rail_->set_items(continue_.size(), [this](Ui &, size_t i) -> std::unique_ptr<View> {
            const auto &h = continue_[i];
            TileSpec t;
            t.title = display_name(h.name);
            t.icon_url = ui::icon(platform::file_kind(h.name) == platform::FileKind::Audio ? "music" : "film");
            t.tint = ui::tint_for(h.name);
            t.badge = extension_upper(h.name);
            const bool remote = h.path.find("://") != std::string::npos;
            // A local video's thumbnail (the cache knows a file by its path, size and date).
            struct stat st {};
            if (!remote && s_.thumbnails != nullptr && platform::file_kind(h.name) == platform::FileKind::Video &&
                ::stat(h.path.c_str(), &st) == 0) {
                platform::FileEntry e;
                e.name = h.name;
                e.path = h.path;
                e.kind = platform::FileKind::Video;
                e.size = static_cast<uint64_t>(st.st_size);
                e.modified_s = static_cast<int64_t>(st.st_mtime);
                t.image_url = s_.thumbnails->get(e);
            }
            t.subtitle = "From " + clock_text(h.position_ms) + (remote ? " · network" : "");
            if (h.length_ms > h.position_ms) t.corner = std::to_string((h.length_ms - h.position_ms + 59'999) / 60'000) + " min left";
            return std::make_unique<Tile>(t);
        });
        recent_source_->set_text("Where you stopped");
        recent_empty_->set_visible(false);
        return;
    }
    rail_->set_items(recent_.size(), [this](Ui &, size_t i) -> std::unique_ptr<View> {
        const auto &e = recent_[i];
        TileSpec t = entry_tile(e, s_.thumbnails);
        t.subtitle = base_name(parent_of(e.path)) + " · " + format_bytes(e.size);
        return std::make_unique<Tile>(t);
    });
    recent_source_->set_text(recent_.empty() ? "" : "Newest videos on your drives");
    recent_empty_->set_visible(recent_.empty() && recent_loaded_);
    if (recent_.empty()) {
        recent_empty_->set_text(volumes_.empty() ? "Plug in a USB drive to see its newest videos here."
                                                 : "No videos found on the connected drives.");
    }
}

// ==== BrowsePage =====================================================================================

BrowsePage::BrowsePage(Services &services) : s_(services) {
    name = "page.browse";
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
    grid_->name = "browse.grid";
    grid_->on_activate = [this](Ui &ui, size_t i) { activate(ui, i); };
    message_ = &body.add<Label>("");
    message_->set_font_size(24);
    message_->set_muted(true);
    message_->set_max_lines(3);
    message_->set_text_align(Align::Center);
    message_->set_align(Align::Center, Align::Center);
    message_->set_visible(false);
}

void BrowsePage::on_shown(Ui &ui) {
    if (!started_ || folder_.empty()) {
        started_ = true;
        show_volumes(ui);
    }
}

void BrowsePage::update(Ui &ui, float dt) {
    Page::update(ui, dt);
    if (s_.thumbnails != nullptr && s_.thumbnails->generation() != thumbs_gen_) {  // new thumbnails
        thumbs_gen_ = s_.thumbnails->generation();
        if (!folder_.empty() && !entries_.empty()) {
            const bool had = ui.has_focus_within(grid_);
            const size_t at = grid_->focused_index();
            grid_->set_items(entries_.size(), [this](Ui &, size_t i) -> std::unique_ptr<View> {
                return std::make_unique<Tile>(entry_tile(entries_[i], s_.thumbnails));
            });
            if (had) grid_->focus_index(ui, at);
        }
    }
    poll_ += dt;
    if (poll_ < kPollSeconds) return;
    poll_ = 0;
    auto vols = media_volumes();
    if (same_volumes(vols, volumes_)) return;
    if (folder_.empty()) {
        show_volumes(ui);
    } else {
        volumes_ = std::move(vols);
        if (volume_of(folder_) == nullptr) {
            ui.toast("The drive was removed.");
            open(ui, "");
        }
    }
}

const platform::RemovableVolume *BrowsePage::volume_of(const std::string &path) const {
    const platform::RemovableVolume *best = nullptr;
    for (const auto &v : volumes_) {
        if (path.compare(0, v.path.size(), v.path) != 0) continue;
        if (path.size() != v.path.size() && path[v.path.size()] != '/') continue;
        if (best == nullptr || v.path.size() > best->path.size()) best = &v;
    }
    return best;
}

void BrowsePage::set_crumbs(const std::string &folder) {
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
    const auto *vol = volume_of(folder);
    crumb("USB drives", folder.empty() || vol == nullptr);
    if (folder.empty() || vol == nullptr) return;
    std::vector<std::string> parts{vol->name};
    std::string rest = folder.substr(vol->path.size());
    size_t start = 0;
    while (start < rest.size()) {
        size_t slash = rest.find('/', start);
        if (slash == std::string::npos) slash = rest.size();
        if (slash > start) parts.push_back(rest.substr(start, slash - start));
        start = slash + 1;
    }
    // Deep folders: keep the drive and the last two levels.
    if (parts.size() > 3) parts.erase(parts.begin() + 1, parts.end() - 2), parts.insert(parts.begin() + 1, "…");
    for (size_t i = 0; i < parts.size(); ++i) {
        chevron();
        crumb(parts[i], i + 1 == parts.size());
    }
}

void BrowsePage::open(Ui &ui, const std::string &folder, const std::string &focus_path, bool take_focus) {
    started_ = true;
    want_focus_ = want_focus_ || take_focus;
    listing_.cancel();
    listing_ = wnk::async::CancellationSource();
    if (folder.empty()) {
        show_volumes(ui, focus_path);
        return;
    }
    if (volumes_.empty()) volumes_ = media_volumes();
    folder_ = folder;
    set_crumbs(folder_);
    count_->set_text("Loading…");
    wnk::async::run_then_post<std::pair<std::vector<platform::FileEntry>, std::string>>(
        s_.ctx->pool(), s_.ctx->dispatcher(), lifetime_.weak(), listing_.token(),
        [folder](const wnk::async::CancellationToken &) {
            std::pair<std::vector<platform::FileEntry>, std::string> r;
            platform::list_folder(folder, &r.first, &r.second);
            return r;
        },
        [this, focus_path](std::pair<std::vector<platform::FileEntry>, std::string> r) {
            show_entries(s_.ctx->ui(), std::move(r.first), r.second, focus_path);
        });
}

void BrowsePage::show_volumes(Ui &ui, const std::string &focus_path) {
    folder_.clear();
    entries_.clear();
    volumes_ = media_volumes();
    set_crumbs("");
    count_->set_text(volumes_.size() == 1 ? "1 drive" : std::to_string(volumes_.size()) + " drives");
    grid_->set_items(volumes_.size(), [this](Ui &, size_t i) -> std::unique_ptr<View> {
        const auto &v = volumes_[i];
        TileSpec t;
        t.title = v.name;
        t.subtitle = volume_detail(v);
        t.icon_url = ui::icon("usb");
        return std::make_unique<Tile>(t);
    });
    message_->set_text("No USB drive is connected.\nPlug one into the TV and it shows up here.");
    message_->set_visible(volumes_.empty());
    place_focus(ui, focus_path);
}

void BrowsePage::show_entries(Ui &ui, std::vector<platform::FileEntry> entries, const std::string &error,
                              const std::string &focus_path) {
    entries_ = std::move(entries);
    // A DVD or Blu-ray folder (VIDEO_TS or BDMV inside): the disc first, played as one.
    if (const std::string mrl = disc_folder_mrl(folder_, entries_); !mrl.empty()) {
        platform::FileEntry disc;
        disc.name = base_name(folder_);
        disc.path = mrl;
        disc.kind = platform::FileKind::Video;
        entries_.insert(entries_.begin(), std::move(disc));
    }
    const size_t n = entries_.size();
    count_->set_text(n == 1 ? "1 item" : std::to_string(n) + " items");
    grid_->set_items(n, [this](Ui &, size_t i) -> std::unique_ptr<View> {
        return std::make_unique<Tile>(entry_tile(entries_[i], s_.thumbnails));
    });
    if (!error.empty()) {
        message_->set_text("This folder cannot be read.\n" + error);
    } else {
        message_->set_text("This folder is empty.");
    }
    message_->set_visible(n == 0);
    place_focus(ui, focus_path);
    WNK_LOGI(kCat, "browse %s: %zu entries%s%s", folder_.c_str(), n, error.empty() ? "" : ", error: ",
             error.c_str());
}

void BrowsePage::place_focus(Ui &ui, const std::string &focus_path) {
    if (grid_->count() == 0) return;
    if (!want_focus_ && !ui.has_focus_within(this)) return;
    want_focus_ = false;
    size_t index = 0;
    if (!focus_path.empty()) {
        if (folder_.empty()) {
            for (size_t i = 0; i < volumes_.size(); ++i)
                if (volumes_[i].path == focus_path) index = i;
        } else {
            for (size_t i = 0; i < entries_.size(); ++i)
                if (entries_[i].path == focus_path) index = i;
        }
    }
    grid_->focus_index(ui, index);
}

void BrowsePage::activate(Ui &ui, size_t i) {
    if (folder_.empty()) {
        if (i < volumes_.size()) open(ui, volumes_[i].path, {}, true);
        return;
    }
    if (i >= entries_.size()) return;
    const auto e = entries_[i];
    switch (e.kind) {
        case platform::FileKind::Folder: open(ui, e.path, {}, true); break;
        case platform::FileKind::Video:
        case platform::FileKind::Audio:
        case platform::FileKind::Playlist: s_.play(e); break;
        case platform::FileKind::Subtitle:
            ui.toast("Subtitle files are used automatically when the video next to them plays.");
            break;
        default:
            if (is_disc(e)) {
                s_.play(e);
            } else {
                ui.toast("VPlayer cannot open this kind of file yet.");
            }
            break;
    }
}

bool BrowsePage::on_action(Ui &ui, const input::ActionEvent &e) {
    if (e.action != input::Action::Back || folder_.empty()) return Page::on_action(ui, e);
    if (e.phase != input::Phase::Press) return true;
    const auto *vol = volume_of(folder_);
    if (vol == nullptr || folder_ == vol->path) {
        open(ui, "", vol ? vol->path : std::string(), true);
    } else {
        open(ui, parent_of(folder_), folder_, true);
    }
    return true;
}

// ==== PlaceholderPage ================================================================================

PlaceholderPage::PlaceholderPage(const std::string &icon_url, const std::string &title, const std::string &body) {
    auto &col = add<Column>(20);
    col.set_align(Align::Center, Align::Center);
    auto &img = col.add<ImageView>(icon_url);
    img.set_size(96, 96);
    img.set_fit(ImageFit::Contain);
    img.set_align(Align::Center, Align::Start);
    auto &t = col.add<Label>(title);
    t.set_font_size(36);
    t.set_bold(true);
    t.set_align(Align::Center, Align::Start);
    auto &b = col.add<Label>(body);
    b.set_font_size(22);
    b.set_muted(true);
    b.set_max_lines(3);
    b.set_text_align(Align::Center);
    b.set_width(Dim::fixed(900));
    b.set_align(Align::Center, Align::Start);
}

}  // namespace vplayer
