// SPDX-License-Identifier: GPL-3.0-or-later
#include "pages.hpp"

#include "library.hpp"

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

// Disc images play like videos in VLC (DVD and Blu-ray menus included).
bool is_disc_image(const platform::FileEntry &e) {
    const std::string ext = extension_upper(e.name);
    return e.kind == platform::FileKind::Other && (ext == "ISO" || ext == "IMG");
}

TileSpec entry_tile(const platform::FileEntry &e) {
    TileSpec t;
    if (is_disc_image(e)) {
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
                                      [](Ui &ui) { ui.toast("Opening a stream URL is coming soon."); }, 30);
    stream.set_height(Dim::fill());
    stream.name = "home.stream";

    auto &header = col.add<Row>(16);
    header.set_align(Align::Stretch, Align::Start);
    section_title(header, "Recently added").set_align(Align::Start, Align::End);
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

void HomePage::show_recent(Ui &, std::vector<platform::FileEntry> files) {
    recent_ = std::move(files);
    rail_->set_items(recent_.size(), [this](Ui &, size_t i) -> std::unique_ptr<View> {
        const auto &e = recent_[i];
        TileSpec t = entry_tile(e);
        t.subtitle = base_name(parent_of(e.path)) + " · " + format_bytes(e.size);
        return std::make_unique<Tile>(t);
    });
    recent_source_->set_text(recent_.empty() ? "" : "Newest videos on your drives");
    recent_empty_->set_visible(recent_.empty());
    if (recent_.empty()) {
        recent_empty_->set_text(volumes_.empty() ? "Plug in a USB drive to see its newest videos here."
                                                 : "No videos found on the connected drives.");
    }
    WNK_LOGI(kCat, "home ready: %zu drives, %zu recent videos", volumes_.size(), recent_.size());
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
