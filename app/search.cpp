// SPDX-License-Identifier: GPL-3.0-or-later
#include "search.hpp"

#include "library.hpp"
#include "network.hpp"

#include "wnk/log.hpp"

#include <algorithm>
#include <set>

namespace vplayer {

using namespace wnk::ui;
using wnk::Size;
namespace platform = wnk::platform;
using vplayer::ui::Tile;
using vplayer::ui::TileSpec;
namespace metric = vplayer::ui::metric;

namespace {

constexpr size_t kMaxResults = 200;

// Two-byte UTF-8 Latin letters with accents → their base letter (Latin-1 Supplement, part of
// Latin Extended-A).
char unaccent(unsigned char a, unsigned char b) {
    const unsigned cp = (a & 0x1Fu) << 6 | (b & 0x3Fu);
    if (cp >= 0xC0 && cp <= 0xC5) return 'a';
    if (cp == 0xC7) return 'c';
    if (cp >= 0xC8 && cp <= 0xCB) return 'e';
    if (cp >= 0xCC && cp <= 0xCF) return 'i';
    if (cp == 0xD1) return 'n';
    if ((cp >= 0xD2 && cp <= 0xD6) || cp == 0xD8) return 'o';
    if (cp >= 0xD9 && cp <= 0xDC) return 'u';
    if (cp == 0xDD) return 'y';
    if (cp >= 0xE0 && cp <= 0xE5) return 'a';
    if (cp == 0xE7) return 'c';
    if (cp >= 0xE8 && cp <= 0xEB) return 'e';
    if (cp >= 0xEC && cp <= 0xEF) return 'i';
    if (cp == 0xF1) return 'n';
    if ((cp >= 0xF2 && cp <= 0xF6) || cp == 0xF8) return 'o';
    if (cp >= 0xF9 && cp <= 0xFC) return 'u';
    if (cp == 0xFD || cp == 0xFF) return 'y';
    if (cp >= 0x100 && cp <= 0x105) return 'a';
    if (cp >= 0x106 && cp <= 0x10D) return 'c';
    if (cp >= 0x112 && cp <= 0x11B) return 'e';
    if (cp >= 0x141 && cp <= 0x142) return 'l';
    if (cp >= 0x143 && cp <= 0x148) return 'n';
    if (cp >= 0x15A && cp <= 0x161) return 's';
    if (cp >= 0x179 && cp <= 0x17E) return 'z';
    return 0;
}

std::vector<std::string> words(const std::string &folded) {
    std::vector<std::string> out;
    std::string w;
    for (const char c : folded + " ") {
        if (c == ' ') {
            if (!w.empty()) out.push_back(w);
            w.clear();
        } else {
            w += c;
        }
    }
    return out;
}

}  // namespace

std::string search_fold(const std::string &s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        const auto c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) {
            out += (c == '.' || c == '_' || c == '-') ? ' ' : static_cast<char>(std::tolower(c));
        } else if (i + 1 < s.size() && (c & 0xE0) == 0xC0) {
            const char base = unaccent(c, static_cast<unsigned char>(s[i + 1]));
            if (base != 0) {
                out += base;
            } else {
                out += s[i];
                out += s[i + 1];
            }
            ++i;
        } else {
            out += s[i];
        }
    }
    return out;
}

bool search_match(const std::string &name, const std::string &query) {
    const auto q = words(search_fold(query));
    if (q.empty()) return false;
    const std::string n = search_fold(name);
    return std::all_of(q.begin(), q.end(), [&](const std::string &w) { return n.find(w) != std::string::npos; });
}

SearchScreen::SearchScreen(Services &services) : Screen("search"), s_(services) {
    set_background(ui::col::background);
    set_padding(Insets{metric::screen_x, metric::screen_y, metric::screen_x, metric::screen_y});
    auto &col = add<Column>(24);
    auto &top = col.add<Row>(20);
    top.set_height(Dim::fixed(76));
    auto &icon = top.add<ImageView>(ui::icon("search"));  // Back on the remote leaves the search
    icon.set_size(44, 44);
    icon.set_fit(ImageFit::Contain);
    icon.set_align(Align::Start, Align::Center);
    field_ = &top.add<TextField>("Search your drives and what you played");
    field_->set_width(Dim::fill());
    field_->set_align(Align::Start, Align::Center);
    field_->set_on_change([this](Ui &u, const std::string &text) {
        query_ = text;
        refresh(u);
    });
    field_->set_on_submit([this](Ui &u, const std::string &text) {
        query_ = text;
        refresh(u);
        if (!results_.empty()) grid_->focus_index(u, 0);
    });
    count_ = &top.add<Label>("");
    count_->set_font_size(20);
    count_->set_muted(true);
    count_->set_align(Align::End, Align::Center);

    auto &body = col.add<View>();
    body.set_height(Dim::fill());
    grid_ = &body.add<CollectionView>(Axis::Vertical, 5, Size{metric::tile_w, metric::tile_h}, metric::tile_gap);
    grid_->set_content_padding(Insets{12, 20, 12, 12});
    grid_->name = "search.grid";
    grid_->on_activate = [this](Ui &ui, size_t i) { activate(ui, i); };
    message_ = &body.add<Label>("Type a name: a film, a series, a song, a playlist.");
    message_->set_font_size(24);
    message_->set_muted(true);
    message_->set_text_align(Align::Center);
    message_->set_align(Align::Center, Align::Center);
}

void SearchScreen::on_show(Ui &ui) {
    Screen::on_show(ui);
    if (!indexed_) index();
    ui.focus(field_);
}

void SearchScreen::index() {
    indexed_ = true;
    count_->set_text("Indexing…");
    // History first (network files included), then the drives.
    std::vector<platform::FileEntry> hist;
    if (s_.history != nullptr) {
        for (const auto &h : s_.history->entries()) {
            platform::FileEntry e;
            e.name = h.name;
            e.path = h.path;
            e.kind = platform::file_kind(h.name) == platform::FileKind::Audio ? platform::FileKind::Audio : platform::FileKind::Video;
            hist.push_back(std::move(e));
        }
    }
    wnk::async::run_then_post<std::vector<platform::FileEntry>>(
        s_.ctx->pool(), s_.ctx->dispatcher(), lifetime_.weak(), wnk::async::CancellationToken{},
        [hist](const wnk::async::CancellationToken &) {
            auto out = hist;
            std::set<std::string> seen;
            for (const auto &e : out) seen.insert(e.path);
            for (auto &e : scan_files(media_volumes(),
                                      {platform::FileKind::Video, platform::FileKind::Audio, platform::FileKind::Playlist}, 8,
                                      60000))
                if (seen.insert(e.path).second) out.push_back(std::move(e));
            return out;
        },
        [this](std::vector<platform::FileEntry> all) {
            all_ = std::move(all);
            WNK_LOGI("search", "%zu entries indexed", all_.size());
            refresh(s_.ctx->ui());
        });
}

void SearchScreen::refresh(Ui &) {
    results_.clear();
    for (const auto &e : all_) {
        if (results_.size() >= kMaxResults) break;
        if (search_match(e.name, query_)) results_.push_back(e);
    }
    grid_->set_items(results_.size(), [this](Ui &, size_t i) -> std::unique_ptr<View> {
        const auto &e = results_[i];
        TileSpec t;
        t.title = display_name(e.name);
        const bool remote = e.path.find("://") != std::string::npos;
        const std::string where = remote ? mrl_protocol(e.path) : e.path.substr(0, e.path.rfind('/'));
        t.subtitle = where.substr(where.rfind('/') == std::string::npos ? 0 : where.rfind('/') + 1);
        t.icon_url = ui::icon(e.kind == platform::FileKind::Audio ? "music" : e.kind == platform::FileKind::Playlist ? "playlist" : "film");
        t.tint = ui::tint_for(e.name);
        t.badge = extension_upper(e.name);
        return std::make_unique<Tile>(t);
    });
    const bool searching = !query_.empty();
    count_->set_text(!indexed_ || all_.empty() ? "" : searching ? std::to_string(results_.size()) + (results_.size() >= kMaxResults ? "+" : "") + " found" : std::to_string(all_.size()) + " files");
    message_->set_text(searching ? "Nothing matches “" + query_ + "”." : "Type a name: a film, a series, a song, a playlist.");
    message_->set_visible(results_.empty());
}

void SearchScreen::activate(Ui &ui, size_t i) {
    if (i >= results_.size()) return;
    const auto e = results_[i];
    if (e.kind == platform::FileKind::Playlist) {
        ui.toast("Playlists open in the Playlists tab.");
        return;
    }
    s_.play(e);
}

}  // namespace vplayer
