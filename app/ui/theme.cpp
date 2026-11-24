// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/theme.hpp"

#include "wnk/render/renderer.hpp"

#include <algorithm>
#include <utility>

namespace vplayer::ui {

using namespace wnk::ui;
using wnk::Point;
using wnk::Rect;
using wnk::Size;
namespace input = wnk::input;
namespace render = wnk::render;

Theme make_theme() {
    Theme t;
    t.background = col::background;
    t.surface = col::surface;
    t.raised = col::raised;
    t.raised_focused = Color::hex(0x2E2E35FF);
    t.text = col::text;
    t.text_muted = col::muted;
    t.text_disabled = Color::hex(0x52525BFF);
    t.accent = col::accent;
    t.focus_ring = Color::hex(0xFFFFFFFF);  // white ring, as in the mockups
    t.scrim = Color::hex(0x050507B8);
    t.placeholder = col::raised;
    t.radius = 20;
    t.ring_width = 4;
    t.ring_gap = 6;
    t.text_size = 24;
    t.small_size = 19;
    t.title_size = 40;
    return t;
}

Color tint_for(const std::string &key) {
    static constexpr Color tints[] = {
        Color::hex(0x2B3A4FFF), Color::hex(0x3B2F45FF), Color::hex(0x2F4538FF), Color::hex(0x3A3328FF),
        Color::hex(0x28343AFF), Color::hex(0x3A2830FF), Color::hex(0x2C2E3EFF), Color::hex(0x33392AFF),
    };
    uint32_t h = 2166136261u;  // FNV-1a
    for (unsigned char c : key) h = (h ^ c) * 16777619u;
    return tints[h % (sizeof tints / sizeof *tints)];
}

namespace {
std::string g_icon_dir;
}

void set_icon_dir(const std::string &dir) { g_icon_dir = dir; }

std::string icon(const char *name) { return "file://" + g_icon_dir + "/" + name + ".svg"; }

// ---- Chip -------------------------------------------------------------------------------------------

Chip::Chip(std::string text, Color background, float size, bool bold) : Label(std::move(text)) {
    set_font_size(size);
    set_bold(bold);
    set_padding(Insets::xy(size * 0.62f, size * 0.25f));
    set_background(background, size * 0.5f);
}

Size Chip::measure_content(Ui &ui, Size available) {
    Size m = Label::measure_content(ui, available);
    m.w = std::min(m.w + 2, available.w);
    return m;
}

// ---- PillTabs ---------------------------------------------------------------------------------------

class PillTabs::Tab : public View {
 public:
    Tab(std::string text, std::function<void(Ui &)> on_pick) : text_(std::move(text)), on_pick_(std::move(on_pick)) {
        set_focusable(true);
        set_focus_ring(true, kHeight / 2);
        accessible_label = text_;
    }
    bool selected = false;

    bool on_action(Ui &ui, const input::ActionEvent &e) override {
        if (e.action != input::Action::Confirm) return false;
        if (e.phase == input::Phase::Press) on_pick_(ui);
        return true;
    }

 protected:
    static constexpr float kHeight = 50, kPadX = 28, kSize = 22;

    render::TextStyle style(const Ui &ui) const {
        render::TextStyle s;
        s.font = selected ? ui.theme().bold : ui.theme().regular;
        s.size = kSize;
        s.color = selected ? col::background : col::text_soft;
        return s;
    }
    Size measure_content(Ui &ui, Size) override {
        // Measured in the bold face either way, so selecting a tab never shifts its neighbours.
        render::TextStyle s = style(ui);
        s.font = ui.theme().bold;
        return {ui.renderer().measure_text(s, text_) + 2 * kPadX, kHeight};
    }
    void on_focus_changed(Ui &ui, bool focused) override {
        if (focused && ui.focus_source() != FocusSource::Pointer) on_pick_(ui);
    }
    void draw(Ui &ui) override {
        render::Renderer &r = ui.renderer();
        const Rect f = frame();
        if (selected) r.fill_rect(f, ui.fade(col::text), f.h / 2);
        render::TextStyle s = style(ui);
        s.color = ui.fade(s.color);
        const float w = r.measure_text(s, text_);
        const float lh = r.line_height(s);
        r.draw_text(s, {f.x + (f.w - w) / 2, f.y + (f.h - lh) / 2 + r.ascent(s)}, text_);
    }

 private:
    std::string text_;
    std::function<void(Ui &)> on_pick_;
};

PillTabs::PillTabs(std::vector<std::string> titles, Handler on_select) : Row(6), on_select_(std::move(on_select)) {
    set_padding(Insets::all(6));
    set_background(col::surface, 31);
    set_focus_scope(false, true);
    for (size_t i = 0; i < titles.size(); ++i) {
        const int index = static_cast<int>(i);
        auto &t = add<Tab>(titles[i], [this, index](Ui &ui) { select(ui, index); });
        t.name = "tab." + titles[i];
        tabs_.push_back(&t);
    }
    if (!tabs_.empty()) tabs_[0]->selected = true;
}

View &PillTabs::tab(int index) { return *tabs_.at(static_cast<size_t>(index)); }

void PillTabs::set_selected(int index) {
    if (index < 0 || index >= static_cast<int>(tabs_.size())) return;
    for (size_t i = 0; i < tabs_.size(); ++i) tabs_[i]->selected = static_cast<int>(i) == index;
    selected_ = index;
}

void PillTabs::select(Ui &ui, int index) {
    if (index == selected_) return;
    set_selected(index);
    if (on_select_) on_select_(ui, index);
}

// ---- IconButton -------------------------------------------------------------------------------------

IconButton::IconButton(std::string icon_url, std::string label, ClickHandler on_click, float size)
    : label_(std::move(label)), on_click_(std::move(on_click)) {
    set_size(size, size);
    set_background(col::surface, size / 2);
    set_focusable(true);
    set_focus_ring(true, size / 2);
    auto &img = add<ImageView>(std::move(icon_url));
    img.set_fit(ImageFit::Contain);
    img.set_size(size * 0.46f, size * 0.46f);
    img.set_align(Align::Center, Align::Center);
}

bool IconButton::on_action(Ui &ui, const input::ActionEvent &e) {
    if (e.action != input::Action::Confirm || !on_click_) return false;
    if (e.phase == input::Phase::Press) {
        ClickHandler h = on_click_;
        h(ui);
    }
    return true;
}

// ---- Tile -------------------------------------------------------------------------------------------

Tile::Tile(const TileSpec &spec) : Column(12), title_(spec.title), subtitle_(spec.subtitle) {
    set_focusable(true);
    set_focus_ring(true, 22);

    auto &thumb = add<View>();
    thumb_ = &thumb;
    thumb.set_height(Dim::fixed(metric::tile_thumb_h));
    thumb.set_background(spec.tint, 22);
    thumb.set_padding(Insets::all(12));
    if (!spec.icon_url.empty()) {
        auto &img = thumb.add<ImageView>(spec.icon_url);
        img.set_fit(ImageFit::Contain);
        img.set_size(64, 64);
        img.set_align(Align::Center, Align::Center);
    }
    if (!spec.badge.empty()) thumb.add<Chip>(spec.badge).set_align(Align::Start, Align::Start);
    if (!spec.corner.empty()) thumb.add<Chip>(spec.corner).set_align(Align::End, Align::End);

    auto &text = add<Column>(4);
    auto &t = text.add<Label>(spec.title);
    t.set_bold(true);
    t.set_font_size(21);
    auto &s = text.add<Label>(spec.subtitle);
    s.set_font_size(17);
    s.set_muted(true);
}

Rect Tile::focus_bounds() const { return thumb_->frame(); }

void Tile::on_focus_changed(Ui &, bool focused) { set_translation({0, focused ? -6.0f : 0.0f}); }

// ---- HeroCard ---------------------------------------------------------------------------------------

HeroCard::HeroCard(std::string icon_url, std::string kicker, std::string title, std::string detail, Color tint,
                   ClickHandler on_click, float title_size)
    : on_click_(std::move(on_click)) {
    // Large cards (the hero) put the icon top left and the text at the bottom; compact ones (the
    // shortcuts beside it) are too short for that and put the icon on the right instead.
    const bool compact = title_size < 40;
    set_focusable(true);
    set_focus_ring(true, 28);
    set_background(tint, 28);
    set_padding(compact ? Insets::xy(32, 26) : Insets::all(36));
    if (!icon_url.empty()) {
        auto &img = add<ImageView>(std::move(icon_url));
        img.set_fit(ImageFit::Contain);
        const float s = compact ? 52 : title_size * 1.5f;
        img.set_size(s, s);
        img.set_align(compact ? Align::End : Align::Start, compact ? Align::Center : Align::Start);
    }
    auto &text = add<Column>(compact ? 6 : 10);
    text.set_align(Align::Stretch, compact ? Align::Center : Align::End);
    if (compact) text.set_padding(Insets{0, 0, 72, 0});  // clear of the icon
    kicker_ = &text.add<Label>(std::move(kicker));
    kicker_->set_font_size(compact ? 16 : 18);
    kicker_->set_bold(true);
    kicker_->set_color(col::accent);
    title_ = &text.add<Label>(std::move(title));
    title_->set_font_size(title_size);
    title_->set_bold(true);
    detail_ = &text.add<Label>(std::move(detail));
    detail_->set_font_size(compact ? 19 : 20);
    detail_->set_color(col::text_soft);
}

void HeroCard::set_texts(const std::string &kicker, const std::string &title, const std::string &detail) {
    kicker_->set_text(kicker);
    title_->set_text(title);
    detail_->set_text(detail);
}

std::string HeroCard::accessible_text() const { return title_->text() + ". " + detail_->text(); }

bool HeroCard::on_action(Ui &ui, const input::ActionEvent &e) {
    if (e.action != input::Action::Confirm || !on_click_) return false;
    if (e.phase == input::Phase::Press) {
        ClickHandler h = on_click_;
        h(ui);
    }
    return true;
}

}  // namespace vplayer::ui
