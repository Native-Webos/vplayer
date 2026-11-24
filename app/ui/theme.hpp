// SPDX-License-Identifier: GPL-3.0-or-later
//
// VPlayer's look (the approved mockups: claude.ai/artifact/G2ZZQ1qBnYBgtGmEU6NAQ6) as a kit Theme
// plus the handful of components the screens are built from. Everything is in the kit's logical
// 1920x1080 units.
#pragma once

#include "wnk/render/color.hpp"
#include "wnk/ui/ui.hpp"
#include "wnk/ui/widgets.hpp"

#include <functional>
#include <string>
#include <vector>

namespace vplayer::ui {

using wnk::render::Color;

// ---- design tokens ---------------------------------------------------------------------------------
namespace col {
inline constexpr Color background = Color::hex(0x0A0A0CFF);
inline constexpr Color surface = Color::hex(0x16161AFF);      // cards, tab bar, icon buttons
inline constexpr Color raised = Color::hex(0x202026FF);
inline constexpr Color text = Color::hex(0xF5F5F7FF);
inline constexpr Color text_soft = Color::hex(0xD4D4D8FF);    // inactive tabs
inline constexpr Color muted = Color::hex(0xA1A1AAFF);        // 8:1 on background
inline constexpr Color accent = Color::hex(0xFF8A00FF);
inline constexpr Color on_accent = Color::hex(0x121212FF);
inline constexpr Color chip = Color::hex(0x0A0A0CC0);         // badges over thumbnails
}  // namespace col

namespace metric {
inline constexpr float screen_x = 96, screen_y = 56;  // TV-safe margins
inline constexpr float top_bar_h = 64;
inline constexpr float tile_w = 316, tile_h = 262, tile_thumb_h = 186, tile_gap = 28;
}  // namespace metric

wnk::ui::Theme make_theme();

// Muted tints for thumbnail placeholders, picked from a name so a file keeps its colour.
Color tint_for(const std::string &key);

// "file:///<app>/icons/<name>.svg"; set_icon_dir once at start.
void set_icon_dir(const std::string &dir);
std::string icon(const char *name);

// ---- components ------------------------------------------------------------------------------------

// A label with a rounded background: format badges over thumbnails, breadcrumb segments.
class Chip : public wnk::ui::Label {
 public:
    explicit Chip(std::string text, Color background = col::chip, float size = 16, bool bold = true);

 protected:
    // A little slack: layout can hand the label a frame a hair narrower than its measured text, and
    // Label then ellipsises a word that fits ("Movi…").
    wnk::Size measure_content(wnk::ui::Ui &ui, wnk::Size available) override;
};

// Pill-shaped tabs in a rounded tray. Moving focus onto a tab selects it (as the kit's Tabs do);
// OK or a click selects it too.
class PillTabs : public wnk::ui::Row {
 public:
    using Handler = std::function<void(wnk::ui::Ui &, int)>;
    PillTabs(std::vector<std::string> titles, Handler on_select);
    void set_selected(int index);  // no handler call
    int selected() const { return selected_; }
    View &tab(int index);

 private:
    class Tab;
    void select(wnk::ui::Ui &ui, int index);
    std::vector<Tab *> tabs_;
    int selected_ = 0;
    Handler on_select_;
};

// A round button holding one icon (search, settings).
class IconButton : public wnk::ui::View {
 public:
    IconButton(std::string icon_url, std::string label, wnk::ui::ClickHandler on_click, float size = 56);
    std::string accessible_text() const override { return label_; }
    bool on_action(wnk::ui::Ui &ui, const wnk::input::ActionEvent &e) override;

 private:
    std::string label_;
    wnk::ui::ClickHandler on_click_;
};

// What a tile shows: a rounded thumbnail (a tint with an icon until real thumbnails exist), badges,
// a title and a line under it.
struct TileSpec {
    std::string title;
    std::string subtitle;
    std::string icon_url;      // centred in the thumbnail; empty for none
    std::string image_url;     // fills the thumbnail (a video frame, an album cover); replaces the icon
    Color tint = col::surface;
    std::string badge;         // top left, e.g. "MKV"
    std::string corner;        // bottom right, e.g. "4.2 GB"
};

// A focusable media tile for grids and rails. Focus lifts it slightly and rings the thumbnail.
class Tile : public wnk::ui::Column {
 public:
    explicit Tile(const TileSpec &spec);
    wnk::Rect focus_bounds() const override;
    std::string accessible_text() const override { return title_ + ", " + subtitle_; }

 protected:
    void on_focus_changed(wnk::ui::Ui &ui, bool focused) override;

 private:
    View *thumb_ = nullptr;
    std::string title_, subtitle_;
};

// A large focusable card: an icon, a small coloured kicker line, a big title and a detail line.
class HeroCard : public wnk::ui::View {
 public:
    HeroCard(std::string icon_url, std::string kicker, std::string title, std::string detail, Color tint,
             wnk::ui::ClickHandler on_click, float title_size = 44);
    void set_texts(const std::string &kicker, const std::string &title, const std::string &detail);
    std::string accessible_text() const override;
    bool on_action(wnk::ui::Ui &ui, const wnk::input::ActionEvent &e) override;

 private:
    wnk::ui::Label *kicker_, *title_, *detail_;
    wnk::ui::ClickHandler on_click_;
};

}  // namespace vplayer::ui
