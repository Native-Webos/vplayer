// SPDX-License-Identifier: GPL-3.0-or-later
#include "music_page.hpp"

#include "library.hpp"

#include "wnk/log.hpp"

#include <algorithm>
#include <random>

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
constexpr const char *kCat = "music";
constexpr float kPollSeconds = 2;
std::string file_url(const std::string &path) { return path.empty() ? std::string() : "file://" + path; }
}  // namespace

MusicPage::MusicPage(Services &services) : s_(services) {
    name = "page.music";
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
    grid_->name = "music.grid";
    grid_->on_activate = [this](Ui &ui, size_t i) { activate(ui, i); };
    message_ = &body.add<Label>("");
    message_->set_font_size(24);
    message_->set_muted(true);
    message_->set_max_lines(3);
    message_->set_text_align(Align::Center);
    message_->set_align(Align::Center, Align::Center);
    message_->set_visible(false);
}

void MusicPage::on_shown(Ui &ui) {
    if (!scanned_) {
        scanned_ = true;
        volumes_ = media_volumes();
        count_->set_text("Looking for music…");
        scan();
    }
    if (album_ < 0) show_albums(ui, 0);
}

void MusicPage::update(Ui &ui, float dt) {
    Page::update(ui, dt);
    poll_ += dt;
    if (poll_ < kPollSeconds || !scanned_) return;
    poll_ = 0;
    auto vols = media_volumes();
    if (same_volumes(vols, volumes_)) return;
    volumes_ = std::move(vols);
    if (album_ >= 0) show_albums(ui, 0);
    scan();
}

void MusicPage::scan() {
    if (scanning_) return;
    scanning_ = true;
    auto vols = volumes_;
    wnk::async::run_then_post<std::vector<Album>>(
        s_.ctx->pool(), s_.ctx->dispatcher(), lifetime_.weak(), wnk::async::CancellationToken{},
        [vols](const wnk::async::CancellationToken &) {
            return build_albums(scan_files(vols, {platform::FileKind::Audio, platform::FileKind::Image}, 5, 40000));
        },
        [this](std::vector<Album> albums) {
            scanning_ = false;
            albums_ = std::move(albums);
            WNK_LOGI(kCat, "%zu albums", albums_.size());
            if (album_ < 0 || album_ >= static_cast<int>(albums_.size())) show_albums(s_.ctx->ui(), 0);
        });
}

void MusicPage::show_albums(Ui &ui, size_t focus) {
    album_ = -1;
    crumbs_->clear_children();
    auto &c = crumbs_->add<Chip>("Music", ui::Color::hex(0x2A2A31FF), 21, true);
    c.set_color(col::text);
    c.set_align(Align::Start, Align::Center);
    size_t tracks = 0;
    for (const auto &a : albums_) tracks += a.tracks.size();
    if (!scanning_) count_->set_text(std::to_string(albums_.size()) + " albums · " + std::to_string(tracks) + " tracks");
    const size_t n = albums_.empty() ? 0 : albums_.size() + 1;  // "Shuffle all" first
    grid_->set_items(n, [this](Ui &, size_t i) -> std::unique_ptr<View> {
        TileSpec t;
        if (i == 0) {
            t.title = "Shuffle all";
            t.subtitle = "Every track on your drives";
            t.icon_url = ui::icon("music");
            t.tint = ui::Color::hex(0x3A2410FF);
            return std::make_unique<Tile>(t);
        }
        const Album &a = albums_[i - 1];
        t.title = a.title;
        t.subtitle = (a.artist.empty() ? std::string() : a.artist + " · ") + std::to_string(a.tracks.size()) +
                     (a.tracks.size() == 1 ? " track" : " tracks");
        if (a.cover.empty()) t.icon_url = ui::icon("music"); else t.image_url = file_url(a.cover);
        t.tint = ui::tint_for(a.folder);
        return std::make_unique<Tile>(t);
    });
    message_->set_text(volumes_.empty() ? "Plug in a USB drive with music: each folder of songs shows up as an album."
                                        : scanning_ ? "Looking for music…" : "No music found on the drives.");
    message_->set_visible(n == 0);
    if (n > 0 && ui.has_focus_within(this)) grid_->focus_index(ui, std::min(focus, n - 1));
}

void MusicPage::show_album(Ui &ui, size_t index) {
    if (index >= albums_.size()) return;
    album_ = static_cast<int>(index);
    const Album &a = albums_[index];
    crumbs_->clear_children();
    auto &m = crumbs_->add<Chip>("Music", col::surface, 21, false);
    m.set_color(col::text_soft);
    m.set_align(Align::Start, Align::Center);
    auto &chev = crumbs_->add<ImageView>(ui::icon("chevron-right"));
    chev.set_size(22, 22);
    chev.set_fit(ImageFit::Contain);
    chev.set_align(Align::Start, Align::Center);
    auto &c = crumbs_->add<Chip>(a.artist.empty() ? a.title : a.artist + " · " + a.title, ui::Color::hex(0x2A2A31FF), 21, true);
    c.set_color(col::text);
    c.set_align(Align::Start, Align::Center);
    count_->set_text(std::to_string(a.tracks.size()) + (a.tracks.size() == 1 ? " track" : " tracks"));
    tags_.assign(a.tracks.size(), TrackTags{});
    order_.resize(a.tracks.size());
    for (size_t k = 0; k < order_.size(); ++k) order_[k] = k;
    // Titles, numbers and lengths from the tracks' tags, read on a worker (libVLC).
    if (s_.vlc != nullptr) {
        std::vector<std::string> paths;
        for (const auto &tr : a.tracks) paths.push_back(tr.path);
        const int which = album_;
        wnk::async::run_then_post<std::vector<TrackTags>>(
            s_.ctx->pool(), s_.ctx->dispatcher(), lifetime_.weak(), wnk::async::CancellationToken{},
            [vlc = s_.vlc, paths](const wnk::async::CancellationToken &) { return read_tags(vlc, paths); },
            [this, which](std::vector<TrackTags> tags) {
                if (album_ != which || tags.size() != tags_.size()) return;  // another album meanwhile
                tags_ = std::move(tags);
                order_ = tag_order(tags_);
                Ui &u = s_.ctx->ui();
                const bool had = u.has_focus_within(grid_);
                const size_t at = grid_->focused_index();
                show_tracks(u);
                if (had) grid_->focus_index(u, at);
            });
    }
    show_tracks(ui);
    message_->set_visible(false);
    grid_->focus_index(ui, 0);
}

void MusicPage::show_tracks(Ui &) {
    const Album &a = albums_[static_cast<size_t>(album_)];
    grid_->set_items(a.tracks.size(), [this](Ui &, size_t i) -> std::unique_ptr<View> {
        const Album &al = albums_[static_cast<size_t>(album_)];
        const size_t k = order_[i];
        const auto &tr = al.tracks[k];
        const TrackTags &tg = tags_[k];
        TileSpec t;
        t.title = tg.title.empty() ? display_name(tr.name) : (tg.number > 0 ? std::to_string(tg.number) + ". " : "") + tg.title;
        t.subtitle = tg.artist.empty() ? extension_upper(tr.name) + " · " + format_bytes(tr.size) : tg.artist;
        if (tg.duration_ms > 0) t.corner = clock_text(tg.duration_ms);
        if (al.cover.empty()) t.icon_url = ui::icon("music"); else t.image_url = file_url(al.cover);
        t.tint = ui::tint_for(al.folder);
        return std::make_unique<Tile>(t);
    });
}

void MusicPage::activate(Ui &ui, size_t i) {
    if (album_ < 0) {
        if (i == 0) {  // shuffle everything
            std::vector<platform::FileEntry> all;
            for (const auto &a : albums_) all.insert(all.end(), a.tracks.begin(), a.tracks.end());
            if (all.empty()) return;
            std::shuffle(all.begin(), all.end(), std::mt19937{std::random_device{}()});
            s_.play_queue(std::move(all), 0);
            return;
        }
        show_album(ui, i - 1);
        return;
    }
    // The album in the order shown, from the track chosen.
    const Album &a = albums_[static_cast<size_t>(album_)];
    if (i >= a.tracks.size()) return;
    std::vector<platform::FileEntry> queue;
    for (const size_t k : order_) queue.push_back(a.tracks[k]);
    s_.play_queue(std::move(queue), i);
}

bool MusicPage::on_action(Ui &ui, const input::ActionEvent &e) {
    if (e.action != input::Action::Back || album_ < 0) return Page::on_action(ui, e);
    if (e.phase == input::Phase::Press) show_albums(ui, static_cast<size_t>(album_) + 1);
    return true;
}

}  // namespace vplayer
