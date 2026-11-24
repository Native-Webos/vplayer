// SPDX-License-Identifier: GPL-3.0-or-later
#include "player/player_screen.hpp"

#include "music.hpp"
#include "thumbnails.hpp"

#include "library.hpp"
#include "ui/theme.hpp"

#include "wnk/log.hpp"
#include "wnk/render/renderer.hpp"
#include "wnk/ui/forms.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <sys/stat.h>
#include <utility>

namespace vplayer {

using namespace wnk::ui;
using wnk::Rect;
using wnk::Size;
using wnk::render::Color;
namespace input = wnk::input;
namespace render = wnk::render;
namespace platform = wnk::platform;
namespace col = vplayer::ui::col;

namespace {

constexpr const char *kCat = "player";
constexpr float kHideAfterS = 4;
constexpr int64_t kSkipMs = 10000, kJumpMs = 30000;
constexpr Color kPanel = Color::hex(0x121216D9);
constexpr Color kSheet = Color::hex(0x121216EB);
constexpr Color kButton = Color::hex(0xFFFFFF14);

const float kSpeeds[] = {0.25f, 0.5f, 0.75f, 1.0f, 1.25f, 1.5f, 1.75f, 2.0f, 3.0f, 4.0f};
const std::vector<std::string> kSpeedNames = {"0.25×", "0.5×", "0.75×", "1×", "1.25×", "1.5×", "1.75×", "2×", "3×", "4×"};
const char *const kDeinterlace[] = {"", "yadif", "yadif2x", "bob", "linear", "blend", "x"};
const std::vector<std::string> kDeinterlaceNames = {"Off", "Yadif", "Yadif (2×)", "Bob", "Linear", "Blend", "X"};
const std::vector<std::string> kVolumeNames = {"100 %", "125 %", "150 %", "175 %", "200 %"};
const std::vector<std::string> kRepeatNames = {"Off", "This file", "Folder"};
const std::vector<std::string> kSleepNames = {"Off", "15 min", "30 min", "45 min", "60 min", "90 min", "End of this file"};
const int kSleepMinutes[] = {0, 15, 30, 45, 60, 90, -1};


std::string delay_text(float seconds) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%+.2f s", seconds);
    return std::fabs(seconds) < 0.001f ? std::string("0 s") : std::string(buf);
}

bool is_press(const input::ActionEvent &e) { return e.phase == input::Phase::Press; }
bool is_press_or_repeat(const input::ActionEvent &e) { return e.phase != input::Phase::Release; }

}  // namespace

// ==== controls =======================================================================================

// The seek bar: focused, Left/Right skip (faster while held); OK pauses. Chapter starts are ticks.
class SeekBar : public View {
 public:
    explicit SeekBar(PlayerScreen &screen) : screen_(screen) {
        set_focusable(true);
        set_focus_ring(false);  // draws its own focus state (a bigger knob)
        set_height(Dim::fixed(36));
        accessible_label = "Position";
    }
    float progress = 0;               // 0..1
    std::vector<float> marks;         // chapter starts, 0..1
    float a = -1, b = -1;             // A-B repeat, 0..1

    bool on_action(Ui &ui, const input::ActionEvent &e) override {
        if (e.action == input::Action::Left || e.action == input::Action::Right) {
            if (!is_press_or_repeat(e)) return true;
            const int64_t step = e.held_ms > 3000 ? 60000 : e.held_ms > 1000 ? kJumpMs : kSkipMs;
            screen_.skip(ui, e.action == input::Action::Left ? -step : step);
            return true;
        }
        if (e.action == input::Action::Confirm) {
            if (is_press(e)) screen_.toggle_pause(ui);
            return true;
        }
        return false;
    }

 protected:
    void draw(Ui &ui) override {
        render::Renderer &r = ui.renderer();
        const Rect f = frame();
        const bool focused = ui.is_focused(this);
        const float h = focused ? 8 : 6;
        const Rect track{f.x, f.y + (f.h - h) / 2, f.w, h};
        r.fill_rect(track, ui.fade(Color::hex(0xFFFFFF2E)), h / 2);
        if (a >= 0) {
            const float bx = b >= 0 ? b : progress;
            r.fill_rect({track.x + track.w * a, track.y - 4, track.w * std::max(0.0f, bx - a), h + 8},
                        ui.fade(Color::hex(0xFFFFFF40)), 3);
        }
        const float p = std::clamp(progress, 0.0f, 1.0f);
        r.fill_rect({track.x, track.y, track.w * p, h}, ui.fade(col::accent), h / 2);
        for (float m : marks) {
            if (m <= 0.001f) continue;
            r.fill_rect({track.x + track.w * m - 1.5f, track.y, 3, h}, ui.fade(Color::hex(0x121216E6)));
        }
        const float knob = focused ? 30 : 20;
        r.fill_rect({track.x + track.w * p - knob / 2, f.y + (f.h - knob) / 2, knob, knob}, ui.fade(render::kWhite),
                    knob / 2);
        if (focused) {
            r.stroke_rect({track.x + track.w * p - knob / 2 - 6, f.y + (f.h - knob) / 2 - 6, knob + 12, knob + 12},
                          ui.fade(Color::hex(0xFFFFFF66)), 3, knob / 2 + 6);
        }
    }

 private:
    PlayerScreen &screen_;
};

// The big white play/pause button.
class PlayPauseButton : public View {
 public:
    explicit PlayPauseButton(PlayerScreen &screen) : screen_(screen) {
        set_size(92, 92);
        set_background(render::kWhite, 46);
        set_focusable(true);
        set_focus_ring(true, 46);
        play_ = &add<ImageView>(ui::icon("play-dark"));
        pause_ = &add<ImageView>(ui::icon("pause-dark"));
        for (ImageView *i : {play_, pause_}) {
            i->set_size(38, 38);
            i->set_fit(ImageFit::Contain);
            i->set_align(Align::Center, Align::Center);
        }
        set_paused(false);
    }
    void set_paused(bool paused) {
        play_->set_visible(paused);
        pause_->set_visible(!paused);
        accessible_label = paused ? "Play" : "Pause";
    }
    bool on_action(Ui &ui, const input::ActionEvent &e) override {
        if (e.action != input::Action::Confirm) return false;
        if (is_press(e)) screen_.toggle_pause(ui);
        return true;
    }

 private:
    PlayerScreen &screen_;
    ImageView *play_ = nullptr, *pause_ = nullptr;
};

// A label measured with 2 units of slack: at the TV's 1.5× scale the rounded glyph widths can exceed
// the measured width, and the text was cut to "…" (as theme.cpp's Chip).
class SlackLabel : public Label {
 public:
    using Label::Label;

 protected:
    Size measure_content(Ui &ui, Size available) override {
        Size m = Label::measure_content(ui, available);
        m.w = std::min(m.w + 2, available.w);
        return m;
    }
};

// A round-ended button with text (the speed, "1.25×").
class PillButton : public View {
 public:
    PillButton(std::string text, std::string label, ClickHandler on_click) : on_click_(std::move(on_click)) {
        set_height(Dim::fixed(64));
        set_padding(Insets::xy(24, 0));
        set_background(kButton, 32);
        set_focusable(true);
        set_focus_ring(true, 32);
        accessible_label = std::move(label);
        text_ = &add<SlackLabel>(std::move(text));
        text_->set_font_size(22);
        text_->set_bold(true);
        text_->set_align(Align::Center, Align::Center);
    }
    void set_text(std::string t) { text_->set_text(std::move(t)); }
    bool on_action(Ui &ui, const input::ActionEvent &e) override {
        if (e.action != input::Action::Confirm || !on_click_) return false;
        if (is_press(e)) on_click_(ui);
        return true;
    }

 private:
    Label *text_;
    ClickHandler on_click_;
};

// A choice in a list (an audio track, a chapter): a row with a mark when it is the current one.
class OptionRow : public FormRow {
 public:
    OptionRow(std::string label, bool selected, ClickHandler on_pick, std::string detail = {})
        : FormRow(std::move(label)), selected_(selected), detail_(std::move(detail)), on_pick_(std::move(on_pick)) {}

 protected:
    bool on_action(Ui &ui, const input::ActionEvent &e) override {
        if (e.action != input::Action::Confirm || !on_pick_) return false;
        if (is_press(e)) on_pick_(ui);
        return true;
    }
    void draw_value(Ui &ui, const Rect &area) override {
        render::Renderer &r = ui.renderer();
        float right = area.x + area.w;
        if (selected_) {
            const float d = 22;
            r.fill_rect({right - d, area.y + (area.h - d) / 2, d, d}, ui.fade(col::accent), d / 2);
            r.fill_rect({right - d + 7, area.y + (area.h - d) / 2 + 7, 8, 8}, ui.fade(col::on_accent), 4);
            right -= d + 16;
        }
        if (!detail_.empty()) {
            render::TextStyle s;
            s.font = ui.theme().regular;
            s.size = ui.theme().small_size;
            s.color = ui.fade(col::muted);
            const float w = r.measure_text(s, detail_);
            r.draw_text(s, {right - w, area.y + (area.h - r.line_height(s)) / 2 + r.ascent(s)}, detail_);
        }
    }
    float value_width(Ui &ui) const override {
        float w = selected_ ? 38 : 0;
        if (!detail_.empty()) {
            render::TextStyle s;
            s.font = ui.theme().regular;
            s.size = ui.theme().small_size;
            w += ui.renderer().measure_text(s, detail_) + 8;
        }
        return w;
    }

 private:
    bool selected_;
    std::string detail_;
    ClickHandler on_pick_;
};

// ==== the side panel =================================================================================

class PlayerPanel : public View {
 public:
    enum Tab { TabAudio = 0, TabSubtitles, TabVideo, TabPlayback, TabInfo };

    explicit PlayerPanel(PlayerScreen &screen) : screen_(screen) {
        set_width(Dim::fixed(700));
        set_align(Align::End, Align::Stretch);
        set_padding(Insets::all(32));
        set_background(kSheet, 32);
        set_focus_scope(true, true);  // arrows stay inside while it is open
        auto &col = add<Column>(24);
        tabs_ = &col.add<ui::PillTabs>(std::vector<std::string>{"Audio", "Subtitles", "Video", "Playback", "Info"},
                                       [this](Ui &ui, int t) { show_tab(ui, t, false); });
        tabs_->set_align(Align::Start, Align::Start);
        auto &scroll = col.add<ScrollView>();
        scroll.set_height(Dim::fill());
        scroll_ = &scroll;
        rows_ = &scroll.add<Column>(6);
    }

    int tab() const { return tab_; }

    // The Info tab's numbers (frames, bitrate) change while it is open: rebuild it every second.
    void update(Ui &ui, float dt) override {
        View::update(ui, dt);
        if (!visible() || tab_ != TabInfo) return;
        info_s_ += dt;
        if (info_s_ >= 1) {
            info_s_ = 0;
            refresh(ui);
        }
    }

    void show_tab(Ui &ui, int t, bool focus_rows) {
        tab_ = t;
        tabs_->set_selected(t);
        rebuild(ui);
        scroll_->scroll_to(0, false);
        if (focus_rows) {
            // The tab row is a focus scope that returns to the tab it last held: give it the selected
            // one first, or Up from the rows lands on whichever tab is nearest (and selects it).
            ui.focus(&tabs_->tab(t));
            ui.focus_within(rows_);
        }
    }

    void focus_tab(Ui &ui) { ui.focus(&tabs_->tab(tab_)); }

    bool on_action(Ui &ui, const input::ActionEvent &e) override {
        if (e.action == input::Action::Back) {
            if (is_press(e)) screen_.close_panel(ui);
            return true;
        }
        return View::on_action(ui, e);
    }

 private:
    void section(const std::string &text) {
        auto &l = rows_->add<Label>(text);
        l.set_font_size(18);
        l.set_bold(true);
        l.set_color(col::accent);
        l.set_padding(Insets{6, 14, 0, 2});
    }

    void rebuild(Ui &ui) {
        rows_->clear_children();
        Playback *p = screen_.playback();
        PlayerSettings &st = screen_.settings();
        if (p == nullptr) return;
        switch (tab_) {
            case TabAudio: build_audio(ui, *p, st); break;
            case TabSubtitles: build_subtitles(ui, *p, st); break;
            case TabVideo: build_video(ui, *p, st); break;
            case TabPlayback: build_playback(ui, *p, st); break;
            default: build_info(*p); break;
        }
    }

    // Re-shows the tab, keeping focus on the same row (after a choice that changes the list).
    void refresh(Ui &ui) {
        size_t index = 0;
        for (size_t i = 0; i < rows_->children().size(); ++i)
            if (ui.has_focus_within(rows_->children()[i].get())) index = i;
        rebuild(ui);
        if (index < rows_->children().size()) ui.focus(rows_->children()[index].get());
    }

    void build_audio(Ui &, Playback &p, PlayerSettings &st) {
        section("TRACK");
        const auto tracks = p.audio_tracks();
        const int cur = p.audio_track();
        if (tracks.empty()) rows_->add<ValueRow>("No audio track", "");
        for (const auto &t : tracks) {
            rows_->add<OptionRow>(t.name, t.id == cur, [this, id = t.id](Ui &ui) {
                screen_.playback()->set_audio_track(id);
                refresh(ui);
            });
        }
        section("ADJUST");
        auto &delay = rows_->add<Slider>("Audio delay", -5.0f, 5.0f, 0.05f, st.audio_delay_ms / 1000.0f,
                                         [this](Ui &, float v) {
                                             screen_.settings().audio_delay_ms = std::lround(v * 1000);
                                             screen_.playback()->set_audio_delay_ms(screen_.settings().audio_delay_ms);
                                         });
        delay.set_format(delay_text);
        delay.set_description("Positive when the sound comes too early");
        std::vector<std::string> eq{"Off"};
        for (auto &n : p.equalizer_presets()) eq.push_back(n);
        section("EFFECTS");
        rows_->add<Choice>("Equalizer", eq, st.equalizer + 1, [this](Ui &ui, int i) {
            screen_.settings().equalizer = i - 1;
            screen_.apply_effects(ui);
        });
        rows_->add<Choice>("Volume boost", kVolumeNames, (st.volume - 100) / 25, [this](Ui &ui, int i) {
            screen_.settings().volume = 100 + i * 25;
            screen_.apply_effects(ui);
        });
        rows_->add<Toggle>("Normalise loudness", st.normalize, [this](Ui &ui, bool on) {
            screen_.settings().normalize = on;
            screen_.apply_effects(ui);
        });
        rows_->add<Toggle>("Wider stereo", st.widen, [this](Ui &ui, bool on) {
            screen_.settings().widen = on;
            screen_.apply_effects(ui);
        });
        if (p.video_plane() && st.effects().active())
            rows_->add<ValueRow>("With effects on", "the TV gets decoded stereo PCM", [](Ui &) {});
    }

    void build_subtitles(Ui &, Playback &p, PlayerSettings &st) {
        section("TRACK");
        const int cur = p.subtitle_track();
        for (const auto &t : p.subtitle_tracks()) {
            rows_->add<OptionRow>(t.name, t.id == cur, [this, id = t.id](Ui &ui) {
                screen_.playback()->set_subtitle_track(id);
                refresh(ui);
            });
        }
        rows_->add<ValueRow>("Load a subtitle file", "From this folder", [this](Ui &ui) { pick_subtitle_file(ui); });
        section("ADJUST");
        auto &delay = rows_->add<Slider>("Subtitle delay", -10.0f, 10.0f, 0.05f, st.subtitle_delay_ms / 1000.0f,
                                         [this](Ui &, float v) {
                                             screen_.settings().subtitle_delay_ms = std::lround(v * 1000);
                                             screen_.playback()->set_subtitle_delay_ms(screen_.settings().subtitle_delay_ms);
                                         });
        delay.set_format(delay_text);
        delay.set_description("Positive when the text comes too early");
    }

    void add_to_playlist(Ui &ui) {
        const auto &e = screen_.current();
        const PlaylistItem item{display_name(e.name), e.path, {}, {}, -1};
        auto add = [this, item](Ui &u, const std::string &name) {
            const bool added = my_playlists_add(screen_.my_playlists(), name, item);
            screen_.save_my_playlists();
            u.toast(added ? "Added to " + name : name + " has it already.");
        };
        auto d = std::make_unique<Dialog>("Add to a playlist", display_name(e.name));
        int n = 0;
        for (const auto &l : *screen_.my_playlists()) {
            if (n++ >= 6) break;
            d->add_button(l.name, [add, name = l.name](Ui &u) { add(u, name); });
        }
        d->add_button("New playlist…", [this, add](Ui &u) {
            auto nd = std::make_unique<Dialog>("New playlist", "Its name:");
            auto &field = nd->body().add<TextField>("Weekend films");
            // OK on the keyboard creates it at once.
            field.set_on_submit([add](Ui &u2, const std::string &name) {
                if (name.empty()) return;
                add(u2, name);
                u2.dismiss_dialog(u2.top_dialog());
            });
            nd->add_button("Create", [add, &field](Ui &u2) {
                if (!field.text().empty()) add(u2, field.text());
            });
            nd->add_button("Cancel");
            u.show_dialog(std::move(nd));
            u.focus(&field);
        });
        d->add_button("Cancel");
        ui.show_dialog(std::move(d));
    }

    void ask_time(Ui &ui) {
        Playback *p = screen_.playback();
        if (p == nullptr) return;
        auto d = std::make_unique<Dialog>("Jump to time", "Hours, minutes and seconds: 1:23:45, 83:10, or seconds: 95. "
                                                          "This file is " + clock_text(p->length_ms()) + " long.");
        auto &field = d->body().add<TextField>(clock_text(p->time_ms()));
        auto go = [this](Ui &u, const std::string &text) {
            const int64_t ms = parse_clock(text);
            Playback *pb = screen_.playback();
            if (ms < 0 || pb == nullptr) {
                u.toast("That is not a time (try 1:23:45).");
                return false;
            }
            if (pb->length_ms() > 0 && ms > pb->length_ms()) {
                u.toast("The file is only " + clock_text(pb->length_ms()) + " long.");
                return false;
            }
            pb->seek_ms(ms);
            u.toast("Jumping to " + clock_text(ms));
            screen_.close_panel(u);
            return true;
        };
        field.set_on_submit([go](Ui &u, const std::string &text) {
            if (go(u, text)) u.dismiss_dialog(u.top_dialog());
        });
        d->add_button("Go", [go, &field](Ui &u) { go(u, field.text()); });
        d->add_button("Cancel");
        ui.show_dialog(std::move(d));
        ui.focus(&field);
    }

    void pick_subtitle_file(Ui &ui) {
        const std::string &path = screen_.current().path;
        const std::string folder = path.substr(0, path.rfind('/'));
        std::vector<platform::FileEntry> entries;
        std::string err;
        platform::list_folder(folder, &entries, &err, 2000);
        auto d = std::make_unique<Dialog>("Load a subtitle file");
        int n = 0;
        for (const auto &e : entries) {
            if (e.kind != platform::FileKind::Subtitle || n >= 8) continue;
            ++n;
            d->add_button(e.name, [this, file = e.path, name = e.name](Ui &u) {
                const bool ok = screen_.playback()->add_subtitle_file(file);
                u.toast(ok ? "Loaded " + name : "Could not load " + name);
                refresh(u);
            });
        }
        if (n == 0) {
            ui.toast("No subtitle files in this folder.");
            return;
        }
        d->add_button("Cancel");
        ui.show_dialog(std::move(d));
    }

    void build_video(Ui &, Playback &p, PlayerSettings &st) {
        section("PICTURE");
        if (p.can_snapshot()) rows_->add<ValueRow>("Take a snapshot", "To the USB drive", [this](Ui &ui) { screen_.take_snapshot(ui); });
        std::vector<std::string> aspects;
        for (int i = 0; i < static_cast<int>(AspectMode::Count); ++i) aspects.emplace_back(aspect_name(static_cast<AspectMode>(i)));
        rows_->add<Choice>("Aspect ratio", aspects, static_cast<int>(st.aspect), [this](Ui &, int i) {
            screen_.settings().aspect = static_cast<AspectMode>(i);
            screen_.video().set_aspect(screen_.settings().aspect);
            screen_.playback()->set_picture_area(i, screen_.settings().zoom / 100.0f);
        });
        auto &zoom = rows_->add<Slider>("Zoom", 100, 300, 5, static_cast<float>(st.zoom), [this](Ui &, float v) {
            screen_.settings().zoom = static_cast<int>(v);
            screen_.video().set_zoom(v / 100.0f);
            screen_.playback()->set_picture_area(static_cast<int>(screen_.settings().aspect), v / 100.0f);
        });
        zoom.set_format([](float v) { return std::to_string(static_cast<int>(v)) + " %"; });
        if (p.can_filter_video()) {
            rows_->add<Choice>("Deinterlace", kDeinterlaceNames, st.deinterlace, [this](Ui &, int i) {
                screen_.settings().deinterlace = i;
                screen_.playback()->set_deinterlace(kDeinterlace[i]);
            });
            section("ADJUSTMENTS");
            rows_->add<Toggle>("Adjust colours", st.adjust.enabled, [this](Ui &ui, bool on) {
                screen_.settings().adjust.enabled = on;
                screen_.playback()->set_picture_adjust(screen_.settings().adjust);
                refresh(ui);
            });
            if (st.adjust.enabled) {
                auto slider = [this](const char *name, float lo, float hi, float step, float PictureAdjust::*field) {
                    auto &s = rows_->add<Slider>(name, lo, hi, step, screen_.settings().adjust.*field,
                                                 [this, field](Ui &, float v) {
                                                     screen_.settings().adjust.*field = v;
                                                     screen_.playback()->set_picture_adjust(screen_.settings().adjust);
                                                 });
                    s.set_format([](float v) {
                        char b[16];
                        std::snprintf(b, sizeof b, "%.2f", v);
                        return std::string(b);
                    });
                };
                slider("Brightness", 0, 2, 0.05f, &PictureAdjust::brightness);
                slider("Contrast", 0, 2, 0.05f, &PictureAdjust::contrast);
                slider("Saturation", 0, 3, 0.05f, &PictureAdjust::saturation);
                slider("Gamma", 0.5f, 2, 0.05f, &PictureAdjust::gamma);
                slider("Hue", -180, 180, 5, &PictureAdjust::hue);
            }
        }
        const auto tracks = p.video_tracks();
        if (tracks.size() > 1) {
            section("VIDEO TRACK");
            const int cur = p.video_track();
            for (const auto &t : tracks) {
                rows_->add<OptionRow>(t.name, t.id == cur, [this, id = t.id](Ui &ui) {
                    screen_.playback()->set_video_track(id);
                    refresh(ui);
                });
            }
        }
    }

    void build_playback(Ui &, Playback &p, PlayerSettings &st) {
        if (p.seekable() && screen_.history() != nullptr) {
            // Bookmarks: kept with the file's history entry; OK on one jumps there.
            const std::string path = screen_.current().path;
            section("BOOKMARKS");
            rows_->add<ValueRow>("Add a bookmark here", clock_text(p.time_ms()), [this, path](Ui &ui) {
                Playback *pb = screen_.playback();
                if (pb == nullptr) return;
                const int64_t at = pb->time_ms();
                const bool added = screen_.history()->add_bookmark(path, screen_.current().name, at, screen_.pool());
                ui.toast(added ? "Bookmark at " + clock_text(at) : "There is a bookmark there already.");
                refresh(ui);
            });
            for (const int64_t b : screen_.history()->bookmarks(path)) {
                rows_->add<ValueRow>("Bookmark", clock_text(b), [this, b](Ui &ui) {
                    if (Playback *pb = screen_.playback()) pb->seek_ms(b);
                    screen_.close_panel(ui);
                });
            }
            section("POSITION");
        }
        if (screen_.my_playlists() != nullptr)
            rows_->add<ValueRow>("Add to a playlist", "Your playlists", [this](Ui &ui) { add_to_playlist(ui); });
        if (p.seekable()) {
            rows_->add<ValueRow>("Jump to time", clock_text(p.time_ms()), [this](Ui &ui) { ask_time(ui); });
            rows_->add<ValueRow>("Start from the beginning", "", [this](Ui &ui) {
                screen_.restart_file(ui);
                screen_.close_panel(ui);
            });
        }
        section("SPEED AND REPEAT");
        if (p.can_change_rate()) {
            rows_->add<Choice>("Speed", kSpeedNames, st.speed, [this](Ui &, int i) {
                screen_.settings().speed = i;
                screen_.apply_speed();
            });
        } else {
            rows_->add<ValueRow>("Speed", "1× · not with the TV decoder", [](Ui &ui) {
                ui.toast("Other speeds work with the software decoder only, for now.");
            });
        }
        std::string ab = "Set start (A)";
        if (st.a_ms >= 0 && st.b_ms < 0) ab = "A " + clock_text(st.a_ms) + " · set end (B)";
        if (st.a_ms >= 0 && st.b_ms >= 0) ab = clock_text(st.a_ms) + " → " + clock_text(st.b_ms) + " · OK clears";
        rows_->add<ValueRow>("A-B repeat", ab, [this](Ui &ui) {
            PlayerSettings &s = screen_.settings();
            const int64_t t = screen_.playback()->time_ms();
            if (s.a_ms < 0) {
                s.a_ms = t;
            } else if (s.b_ms < 0 && t > s.a_ms + 500) {
                s.b_ms = t;
            } else {
                s.a_ms = s.b_ms = -1;
            }
            refresh(ui);
        });
        rows_->add<Choice>("Repeat", kRepeatNames, st.repeat, [this](Ui &, int i) { screen_.settings().repeat = i; });
        rows_->add<Toggle>("Play the next file automatically", st.autoplay,
                           [this](Ui &, bool on) { screen_.settings().autoplay = on; });
        std::string sleep_label = "Sleep timer";
        if (st.sleep_left_s > 0) sleep_label += " · " + clock_text(static_cast<int64_t>(st.sleep_left_s * 1000)) + " left";
        rows_->add<Choice>(sleep_label, kSleepNames, st.sleep, [this](Ui &ui, int i) {
            PlayerSettings &s = screen_.settings();
            s.sleep = i;
            s.sleep_left_s = kSleepMinutes[i] > 0 ? kSleepMinutes[i] * 60.0f : 0;
            ui.toast(i == 0 ? "Sleep timer off" : "The player stops after " + kSleepNames[static_cast<size_t>(i)]);
        });
        const auto titles = p.titles();
        if (!titles.empty()) {
            section("TITLES");
            // The longest non-menu title is the main film.
            size_t main = titles.size();
            for (size_t k = 0; k < titles.size(); ++k)
                if (!titles[k].menu && (main == titles.size() || titles[k].length_ms > titles[main].length_ms)) main = k;
            const int cur = p.title();
            for (size_t k = 0; k < titles.size(); ++k) {
                std::string detail = titles[k].length_ms > 0 ? clock_text(titles[k].length_ms) : std::string();
                if (k == main) detail = "Main film · " + detail;
                rows_->add<OptionRow>(titles[k].name, static_cast<int>(k) == cur,
                                      [this, k](Ui &ui) {
                                          screen_.playback()->set_title(static_cast<int>(k));
                                          refresh(ui);
                                      },
                                      detail);
            }
        }
        const auto chapters = p.chapters();
        if (!chapters.empty()) {
            section("CHAPTERS");
            const int cur = p.chapter();
            for (size_t i = 0; i < chapters.size(); ++i) {
                rows_->add<OptionRow>(chapters[i].name, static_cast<int>(i) == cur,
                                      [this, i](Ui &ui) {
                                          screen_.playback()->set_chapter(static_cast<int>(i));
                                          refresh(ui);
                                      },
                                      clock_text(chapters[i].start_ms));
            }
        }
    }

    void build_info(Playback &p) {
        section("FILE");
        rows_->add<ValueRow>("Name", screen_.current().name);
        if (screen_.current().size > 0) rows_->add<ValueRow>("Size", format_bytes(screen_.current().size));
        rows_->add<ValueRow>("Length", clock_text(p.length_ms()));
        section("STREAMS");
        for (const auto &[k, v] : p.info()) rows_->add<ValueRow>(k, v);
    }

    PlayerScreen &screen_;
    ui::PillTabs *tabs_ = nullptr;
    ScrollView *scroll_ = nullptr;
    Column *rows_ = nullptr;
    int tab_ = 0;
    float info_s_ = 0;
};

// ==== PlayerScreen ===================================================================================

PlayerScreen::PlayerScreen(Services &services, Navigator &nav, Opener open, std::vector<platform::FileEntry> queue,
                           size_t index)
    : Screen("player"), s_(services), nav_(nav), open_(std::move(open)), queue_(std::move(queue)),
      index_(std::min(index, queue_.empty() ? size_t{0} : queue_.size() - 1)) {
    set_background(render::kBlack);
    video_ = &add<VideoView>(nullptr);
    video_->name = "player.video";

    // Audio files: the album's cover (or a music note) where the picture would be.
    art_ = &add<ImageView>("");
    art_->set_size(520, 520);
    art_->set_fit(ImageFit::Contain);
    art_->set_align(Align::Center, Align::Center);
    art_->set_visible(false);

    subs_ = &add<wnk::media::SubtitleView>();
    subs_->name = "player.subtitles";
    subs_->set_visible(false);
    subs_->set_provider([this] {
        return playback_ && !closing_ ? playback_->subtitle_cues() : wnk::media::SubtitleView::Cues{};
    });
    subs_->set_clock([this] {
        return playback_ ? (playback_->time_ms() - settings_.subtitle_delay_ms) * 1000 : int64_t{0};
    });

    catcher_ = &add<View>();
    catcher_->set_focusable(true);
    catcher_->set_focus_ring(false);
    catcher_->set_size(1, 1);
    catcher_->set_align(Align::Start, Align::Start);
    catcher_->name = "player.catcher";

    spinner_ = &add<Spinner>(72);

    overlay_ = &add<View>();
    overlay_->set_padding(Insets{ui::metric::screen_x, ui::metric::screen_y, ui::metric::screen_x, ui::metric::screen_y});

    // ---- top: back, title, format ----
    auto &top = overlay_->add<Row>(20);
    top.set_align(Align::Stretch, Align::Start);
    top.set_height(Dim::fixed(76));
    auto &back = top.add<ui::IconButton>(ui::icon("back"), "Back", [this](Ui &u) { close(u); }, 64);
    back.set_background(kPanel, 32);
    back.set_align(Align::Start, Align::Center);
    back.name = "player.back";
    auto &titles = top.add<Column>(2);
    titles.set_align(Align::Start, Align::Center);
    titles.set_background(kPanel, 20);
    titles.set_padding(Insets::xy(24, 10));
    title_ = &titles.add<Label>("");
    title_->set_font_size(30);
    title_->set_bold(true);
    subtitle_ = &titles.add<Label>("");
    subtitle_->set_font_size(18);
    subtitle_->set_color(col::text_soft);
    top.add<View>().set_width(Dim::fill());
    format_ = &top.add<ui::Chip>("", kPanel, 18);
    format_->set_align(Align::End, Align::Center);
    format_->set_visible(false);

    // ---- bottom: the floating control bar ----
    auto &panel = overlay_->add<Column>(16);
    panel.set_align(Align::Stretch, Align::End);
    panel.set_background(kPanel, 32);
    panel.set_padding(Insets{40, 24, 40, 24});

    auto &info_row = panel.add<Row>(16);
    chapter_ = &info_row.add<Label>("");
    chapter_->set_font_size(20);
    chapter_->set_color(col::text_soft);
    info_row.add<View>().set_width(Dim::fill());
    ends_ = &info_row.add<Label>("");
    ends_->set_font_size(20);
    ends_->set_color(col::text_soft);

    auto &seek_row = panel.add<Row>(24);
    time_ = &seek_row.add<Label>("0:00");
    time_->set_font_size(20);
    time_->set_bold(true);
    time_->set_align(Align::Start, Align::Center);
    seek_ = &seek_row.add<SeekBar>(*this);
    seek_->set_width(Dim::fill());
    seek_->set_align(Align::Stretch, Align::Center);
    seek_->name = "player.seek";
    length_ = &seek_row.add<Label>("");
    length_->set_font_size(20);
    length_->set_color(col::text_soft);
    length_->set_align(Align::End, Align::Center);

    auto &buttons = panel.add<Row>(16);
    auto round = [](Row &row, const char *icon, const char *label, ClickHandler h) -> View & {
        auto &b = row.add<ui::IconButton>(ui::icon(icon), label, std::move(h), 64);
        b.set_background(kButton, 32);
        b.set_align(Align::Start, Align::Center);
        return b;
    };
    auto &left = buttons.add<Row>(16);
    left.set_width(Dim::fill());
    prev_btn_ = &round(left, "prev", "Previous file", [this](Ui &u) { go_to(u, -1); });
    next_btn_ = &round(left, "next", "Next file", [this](Ui &u) { go_to(u, +1); });

    auto &center = buttons.add<Row>(20);
    round(center, "back10", "Back 10 seconds", [this](Ui &u) { skip(u, -kSkipMs); });
    play_ = &center.add<PlayPauseButton>(*this);
    play_->set_align(Align::Start, Align::Center);
    play_->name = "player.play";
    round(center, "fwd10", "Forward 10 seconds", [this](Ui &u) { skip(u, kSkipMs); });

    auto &right = buttons.add<Row>(16);
    right.set_width(Dim::fill());
    right.set_justify(Align::End);
    round(right, "audio", "Audio", [this](Ui &u) { open_panel(u, PlayerPanel::TabAudio); }).name = "player.audio";
    round(right, "captions", "Subtitles", [this](Ui &u) { open_panel(u, PlayerPanel::TabSubtitles); }).name = "player.subs";
    speed_btn_ = &right.add<PillButton>("1×", "Speed", [this](Ui &u) { open_panel(u, PlayerPanel::TabPlayback); });
    speed_btn_->set_align(Align::Start, Align::Center);
    round(right, "aspect", "Picture", [this](Ui &u) { open_panel(u, PlayerPanel::TabVideo); }).name = "player.picture";
    round(right, "more", "More", [this](Ui &u) { open_panel(u, PlayerPanel::TabInfo); }).name = "player.more";

    // ---- the side panel (hidden until opened) ----
    auto &holder = add<View>();  // the panel floats with the screen margin around it
    holder.set_padding(Insets::all(ui::metric::screen_y));
    panel_ = &holder.add<PlayerPanel>(*this);
    panel_->set_visible(false);
    panel_->name = "player.panel";
}

PlayerScreen::~PlayerScreen() = default;

void PlayerScreen::on_show(Ui &ui) {
    Screen::on_show(ui);
    if (s_.thumbnails != nullptr) s_.thumbnails->set_paused(true);  // never competes with playback
    wake_ = s_.ctx->keep_screen_on("video playback");
    start(ui);
    ui.focus(play_);
}

void PlayerScreen::on_hide(Ui &ui) {
    Screen::on_hide(ui);
    if (s_.thumbnails != nullptr) s_.thumbnails->set_paused(false);
    ui.set_clear_color(std::nullopt);
}

void PlayerScreen::start(Ui &ui) {
    if (queue_.empty()) return;
    const auto &e = queue_[index_];
    playback_ = open_(e, software_, settings_.effects());
    if (!playback_) {
        ui.toast("Cannot open " + e.name);
        return;
    }
    // The hardware path draws on the TV's video plane under the UI: clear to transparent there.
    const bool plane = playback_->video_plane();
    video_->set_visible(!plane);
    video_->set_sink(plane ? nullptr : playback_->frames());
    subs_->set_visible(plane);
    subs_->clear_ass_track();
    if (s_.settings != nullptr) {  // Settings: subtitle size and box
        const float scale = subtitle_scale(s_.settings->subtitle_size);
        subs_->set_text_scale(scale);
        subs_->set_text_style(s_.settings->subtitle_box ? wnk::media::SubtitleView::TextStyle::Box
                                                       : wnk::media::SubtitleView::TextStyle::Outline);
        playback_->set_subtitle_look(scale, s_.settings->subtitle_box, s_.settings->subtitle_colour == 1);
    }
    subs_track_ = -2;
    subs_size_ = {0, 0};
    set_background(plane ? std::nullopt : std::optional<Color>(render::kBlack));
    ui.set_clear_color(plane ? std::optional<Color>(render::kTransparent) : std::nullopt);
    title_->set_text(display_name(e.name));
    const bool audio = e.kind == platform::FileKind::Audio && e.path.find("://") == std::string::npos;
    art_->set_visible(audio);
    if (audio) {
        std::vector<platform::FileEntry> siblings;
        std::string err;
        std::vector<std::string> images;
        if (platform::list_folder(e.path.substr(0, e.path.rfind('/')), &siblings, &err, 500))
            for (const auto &x : siblings)
                if (x.kind == platform::FileKind::Image) images.push_back(x.path);
        const std::string cover = pick_cover(images);
        art_->set_url(cover.empty() ? ui::icon("music") : "file://" + cover);
    }
    const std::string folder = e.path.substr(0, e.path.rfind('/'));
    std::string sub = folder.substr(folder.rfind('/') + 1);
    if (queue_.size() > 1) sub += " · " + std::to_string(index_ + 1) + " of " + std::to_string(queue_.size());
    subtitle_->set_text(sub);
    format_->set_visible(false);
    if (!reopen_) {  // a new file: no delays, no A-B (a reopened one keeps them)
        settings_.audio_delay_ms = settings_.subtitle_delay_ms = 0;
        settings_.a_ms = settings_.b_ms = -1;
    }
    ended_ = false;
    history_s_ = 0;
    // A resume point (not for the launch parameter "at", which is set already).
    if (start_at_ms_ < 0 && s_.history != nullptr) {
        const int64_t r = s_.history->resume_ms(e.path);
        if (r > 0) {
            start_at_ms_ = r;
            resume_note_ = true;
        }
    }
    applied_ = reported_ = false;
    prev_btn_->set_enabled(has_previous());
    next_btn_->set_enabled(has_next());
    WNK_LOGI(kCat, "open %zu/%zu: %s", index_ + 1, queue_.size(), e.path.c_str());
}

// Where this file stopped, for its resume point and "Continue watching".
void PlayerScreen::remember_position() {
    if (!playback_ || s_.history == nullptr || !reported_ || queue_.empty()) return;
    const auto &e = queue_[index_];
    HistoryEntry h;
    h.path = e.path;
    h.name = e.name;
    h.length_ms = playback_->length_ms();
    h.position_ms = ended_ ? h.length_ms : playback_->time_ms();
    h.watched = ended_;
    h.played_at_s = static_cast<int64_t>(std::time(nullptr));
    if (h.length_ms <= 0 && !ended_) return;  // a live stream: nothing to resume
    s_.history->record(h, s_.ctx->pool());
}

void PlayerScreen::take_snapshot(Ui &ui) {
    if (!playback_) return;
    // The first drive that can be written to, in a folder of its own.
    const platform::RemovableVolume *target = nullptr;
    const auto vols = media_volumes();
    for (const auto &v : vols)
        if (!v.read_only) {
            target = &v;
            break;
        }
    if (target == nullptr) {
        ui.toast("Plug in a USB drive that can be written to, for snapshots.");
        return;
    }
    const std::string folder = target->path + "/VPlayer snapshots";
    ::mkdir(folder.c_str(), 0755);
    char stamp[32];
    const std::time_t now = std::time(nullptr);
    std::strftime(stamp, sizeof stamp, "%Y-%m-%d %H.%M.%S", std::localtime(&now));
    const std::string file = folder + "/" + display_name(queue_[index_].name) + " " + clock_text(playback_->time_ms()) + " " + stamp + ".png";
    std::string safe = file;
    for (size_t i = folder.size() + 1; i < safe.size(); ++i)
        if (safe[i] == ':' || safe[i] == '/') safe[i] = '.';  // FAT drives refuse ':'
    const bool ok = playback_->snapshot(safe);
    ui.toast(ok ? "Snapshot saved in \"VPlayer snapshots\" on " + target->name : "The snapshot could not be saved.");
}

void PlayerScreen::restart_file(Ui &ui) {
    if (!playback_) return;
    playback_->seek_ms(0);
    if (s_.history != nullptr && !queue_.empty()) s_.history->restart(queue_[index_].path, s_.ctx->pool());
    history_s_ = 0;
    ui.toast("From the beginning.");
}

void PlayerScreen::stop_current() {
    if (!playback_) return;
    remember_position();
    // libVLC's stop is synchronous and can take a moment: do it on a worker. The frame sink stays
    // alive with the video view until both are gone.
    std::shared_ptr<Playback> p(std::move(playback_));
    s_.ctx->pool().submit([p](const wnk::async::CancellationToken &) mutable { p.reset(); });
}

void PlayerScreen::apply_session() {
    // What carries over from the previous file, once this one plays (track lists exist by then).
    applied_ = true;
    apply_speed();
    playback_->apply_audio_effects(settings_.effects());  // live parts (the rest came with the open)
    if (settings_.deinterlace > 0) playback_->set_deinterlace(kDeinterlace[settings_.deinterlace]);
    if (settings_.adjust.enabled) playback_->set_picture_adjust(settings_.adjust);
    video_->set_aspect(settings_.aspect);
    video_->set_zoom(settings_.zoom / 100.0f);
    playback_->set_picture_area(static_cast<int>(settings_.aspect), settings_.zoom / 100.0f);
    if (reopen_) {  // the same file again: the viewer's tracks and delays
        const Reopen r = *reopen_;
        reopen_.reset();
        if (r.audio_track >= 0 && r.audio_track != playback_->audio_track()) playback_->set_audio_track(r.audio_track);
        if (r.subtitle_track != playback_->subtitle_track()) playback_->set_subtitle_track(r.subtitle_track);
        if (r.audio_delay_ms != 0) playback_->set_audio_delay_ms(r.audio_delay_ms);
        if (r.subtitle_delay_ms != 0) playback_->set_subtitle_delay_ms(r.subtitle_delay_ms);
    }
}

void PlayerScreen::apply_effects(Ui &ui) {
    if (!playback_ || playback_->apply_audio_effects(settings_.effects())) return;
    // The engine takes them at open: the same file again, from here.
    start_at_ms_ = playback_->time_ms();
    reopen_ = Reopen{playback_->audio_track(), playback_->subtitle_track(), settings_.audio_delay_ms,
                     settings_.subtitle_delay_ms};
    ui.toast("Applying the audio effects…");
    stop_current();
    start(ui);
    if (!playback_) reopen_.reset();
}

void PlayerScreen::apply_speed() {
    // The choice carries over to the next file; this one may not be able to follow it.
    const bool fixed = playback_ && !playback_->can_change_rate();
    speed_settle_s_ = 1.5f;  // the engine may take a moment, or refuse
    if (playback_ && !fixed) playback_->set_rate(kSpeeds[settings_.speed]);
    speed_btn_->set_text(fixed ? "1×" : kSpeedNames[static_cast<size_t>(settings_.speed)]);
}

void PlayerScreen::go_to(Ui &ui, int delta) {
    const long target = static_cast<long>(index_) + delta;
    if (target < 0 || target >= static_cast<long>(queue_.size())) {
        ui.toast(delta > 0 ? "This is the last file in the folder." : "This is the first file in the folder.");
        return;
    }
    stop_current();
    reopen_.reset();
    index_ = static_cast<size_t>(target);
    software_ = false;
    start(ui);
    show_controls(ui, false);
}

void PlayerScreen::on_descendant_focused(Ui &ui, View *child, View *focused) {
    Screen::on_descendant_focused(ui, child, focused);
    if (focused != catcher_) idle_s_ = 0;
}

void PlayerScreen::toggle_pause(Ui &ui) {
    if (!playback_) return;
    const bool pause = playback_->state() != PlayState::Paused;
    playback_->set_pause(pause);
    play_->set_paused(pause);
    show_controls(ui, false);
}

void PlayerScreen::skip(Ui &ui, int64_t delta_ms) {
    if (!playback_ || !playback_->seekable()) {
        ui.toast("This stream cannot be skipped.");
        return;
    }
    playback_->seek_ms(playback_->time_ms() + delta_ms);
    idle_s_ = 0;
    if (!controls_) show_controls(ui, false);
}

void PlayerScreen::open_panel(Ui &ui, int tab) {
    panel_opener_ = ui.focused();
    overlay_->set_visible(false);
    panel_->set_visible(true);
    panel_->show_tab(ui, tab, true);
    controls_ = true;
    overlay_target_ = overlay_alpha_ = 1;
}

void PlayerScreen::close_panel(Ui &ui) {
    panel_->set_visible(false);
    overlay_->set_visible(true);
    show_controls(ui, false);
    ui.focus(panel_opener_ && panel_opener_ != catcher_ ? panel_opener_ : static_cast<View *>(play_));
    refresh_bar();
}

void PlayerScreen::refresh_bar() { apply_speed(); }

void PlayerScreen::update_subtitles(float dt) {
    // Above the control bar while it shows (not for the side panel), near the bottom otherwise.
    const bool bar = overlay_->visible() && overlay_alpha_ > 0.5f;
    const float lift = bar ? 0.28f : 0.07f;  // the bar's top is about 26 % up
    subs_->set_bottom_margin(lift);
    // SubRip lines drawn by libass take the margin from their header: made again with it.
    if (playback_->set_subtitle_lift(lift)) subs_track_ = -2;

    // The picture's rectangle on screen (the TV fits it into the whole screen), so subtitles sit on
    // the picture and styled ASS scales with it.
    const auto size = playback_->video_size();
    if (size != subs_size_ && size.first > 0 && size.second > 0) {
        subs_size_ = size;
        const Rect f = frame();
        const float scale = std::min(f.w / size.first, f.h / size.second);
        const float w = size.first * scale, h = size.second * scale;
        subs_->set_video_rect({f.x + (f.w - w) / 2, f.y + (f.h - h) / 2, w, h});
        subs_track_ = -2;  // styled ASS needs the size too
    }

    // Styled ASS/SSA through libass when the selected track has a script header. The header comes
    // with the stream, a moment after the track is chosen: plain text shows until then.
    const int track = playback_->subtitle_track();
    if (track != subs_track_) {
        subs_track_ = track;
        subs_ass_done_ = false;
        subs_retry_s_ = 0;
        subs_->clear_ass_track();
    }
    if (subs_ass_done_ || track < 0) return;
    subs_retry_s_ -= dt;
    if (subs_retry_s_ > 0) return;
    subs_retry_s_ = 0.5f;
    const std::string header = playback_->subtitle_ass_header();
    if (header.empty()) return;
    subs_ass_done_ = true;
    const std::string font = s_.ctx->launch().app_dir + "/fonts/Inter-SemiBold.ttf";
    const auto fonts = playback_->subtitle_fonts();
    const bool styled = subs_->set_ass_track(header, fonts, font, size.first > 0 ? size.first : 1920,
                                             size.second > 0 ? size.second : 1080);
    WNK_LOGI(kCat, "subtitles: track %d, ASS %s, %zu attached font(s)", track, styled ? "styled by libass" : "as plain text",
             fonts.size());
}

void PlayerScreen::show_controls(Ui &ui, bool focus_play) {
    idle_s_ = 0;
    overlay_target_ = 1;
    if (!controls_) {
        controls_ = true;
        ui.focus(focus_play ? static_cast<View *>(play_) : static_cast<View *>(seek_));
    }
}

void PlayerScreen::hide_controls(Ui &ui) {
    if (!controls_ || panel_->visible()) return;
    controls_ = false;
    overlay_target_ = 0;
    ui.focus(catcher_);
}

void PlayerScreen::close(Ui &ui) {
    if (closing_) return;
    closing_ = true;
    wake_.release();
    stop_current();
    nav_.pop(ui);
}

void PlayerScreen::ended(Ui &ui) {
    if (settings_.sleep == 6) {  // "End of this file"
        ui.toast("Sleep timer: stopped at the end of the file.");
        close(ui);
        return;
    }
    if (settings_.repeat == 1) {
        stop_current();
        start(ui);
        return;
    }
    if (has_next() && (settings_.autoplay || settings_.repeat == 2)) {
        go_to(ui, +1);
        return;
    }
    if (settings_.repeat == 2 && queue_.size() > 1) {
        stop_current();
        index_ = 0;
        start(ui);
        return;
    }
    close(ui);
}

bool PlayerScreen::on_back(Ui &ui) {
    if (panel_->visible()) {
        close_panel(ui);
        return true;
    }
    close(ui);
    return true;
}

bool PlayerScreen::on_action(Ui &ui, const input::ActionEvent &e) {
    using input::Action;
    switch (e.action) {
        case Action::Back:
            if (is_press(e)) on_back(ui);
            return true;
        case Action::Stop:
            if (is_press(e)) close(ui);
            return true;
        case Action::PlayPause:
            if (is_press(e)) toggle_pause(ui);
            return true;
        case Action::Play:
        case Action::Pause:
            if (is_press(e) && playback_ && (playback_->state() == PlayState::Paused) == (e.action == Action::Play))
                toggle_pause(ui);
            return true;
        case Action::Rewind:
        case Action::FastForward:
            if (is_press_or_repeat(e)) skip(ui, e.action == Action::Rewind ? -kJumpMs : kJumpMs);
            return true;
        case Action::ChannelUp:
        case Action::ChannelDown:
            if (is_press(e)) go_to(ui, e.action == Action::ChannelUp ? +1 : -1);
            return true;
        case Action::Red:
        case Action::Green:
        case Action::Yellow:
        case Action::Blue:
            if (is_press(e)) {
                const int tab = e.action == Action::Red      ? PlayerPanel::TabInfo
                                : e.action == Action::Green  ? PlayerPanel::TabPlayback
                                : e.action == Action::Yellow ? PlayerPanel::TabSubtitles
                                                             : PlayerPanel::TabAudio;
                open_panel(ui, tab);
            }
            return true;
        default: break;
    }
    if (!controls_) {
        // Controls hidden: the catcher has focus and every key lands here.
        switch (e.action) {
            case Action::Left:
            case Action::Right:
                if (is_press_or_repeat(e)) skip(ui, e.action == Action::Left ? -kSkipMs : kSkipMs);
                return true;
            case Action::Confirm:
                if (is_press(e)) toggle_pause(ui);
                return true;
            default:
                if (is_press(e)) show_controls(ui, true);
                return true;
        }
    }
    idle_s_ = 0;
    return Screen::on_action(ui, e);
}

void PlayerScreen::update(Ui &ui, float dt) {
    Screen::update(ui, dt);
    if (!playback_ || closing_) return;
    playback_->tick();
    video_->poll();
    if (playback_->video_plane()) update_subtitles(dt);

    const PlayState state = playback_->state();
    if (state == PlayState::Error && playback_->can_fall_back() && !software_) {
        WNK_LOGW(kCat, "TV decoder refused it (%s): software path", playback_->error().c_str());
        ui.toast("The TV decoder cannot play this file. Playing it in software.");
        stop_current();
        software_ = true;
        start(ui);
        return;
    }
    if (state == PlayState::Error) {
        const std::string err = playback_->error();
        WNK_LOGW(kCat, "playback failed: %s", err.c_str());
        ui.toast("Cannot play this file" + (err.empty() ? std::string(".") : ": " + err));
        if (has_next() && settings_.autoplay) {
            go_to(ui, +1);
        } else {
            close(ui);
        }
        return;
    }
    if (state == PlayState::Ended) {
        WNK_LOGI(kCat, "playback ended");
        ended_ = true;
        ended(ui);
        return;
    }
    if (!applied_ && (state == PlayState::Playing || state == PlayState::Paused)) apply_session();
    // Something on screen: a frame drawn by the app, or (video plane) the TV playing.
    const bool shown = playback_->video_plane() ? state == PlayState::Playing || state == PlayState::Paused
                                                : video_->has_frame();
    if (start_at_ms_ >= 0 && shown && playback_->seekable()) {
        WNK_LOGI(kCat, "starting at %lld ms", static_cast<long long>(start_at_ms_));
        playback_->seek_ms(start_at_ms_);
        if (resume_note_) ui.toast("Continuing from " + clock_text(start_at_ms_) + ". To start over: Playback, then Start from the beginning.", 5000);
        resume_note_ = false;
        start_at_ms_ = -1;
    }
    if (pending_panel_ >= 0 && applied_ && shown) {
        open_panel(ui, pending_panel_);
        pending_panel_ = -1;
    }

    const int64_t t = playback_->time_ms(), len = playback_->length_ms();
    // A-B repeat.
    if (settings_.a_ms >= 0 && settings_.b_ms > settings_.a_ms && t >= settings_.b_ms) playback_->seek_ms(settings_.a_ms);
    // Sleep timer (minutes); "end of file" is handled in ended().
    if (settings_.sleep_left_s > 0 && state == PlayState::Playing) {
        settings_.sleep_left_s -= dt;
        if (settings_.sleep_left_s <= 0) {
            ui.toast("Sleep timer: playback stopped.");
            close(ui);
            return;
        }
    }

    time_->set_text(clock_text(t));
    length_->set_text(len > 0 ? clock_text(len) : "");
    seek_->progress = len > 0 ? static_cast<float>(t) / static_cast<float>(len) : 0;
    seek_->a = settings_.a_ms >= 0 && len > 0 ? static_cast<float>(settings_.a_ms) / len : -1;
    seek_->b = settings_.b_ms >= 0 && len > 0 ? static_cast<float>(settings_.b_ms) / len : -1;
    play_->set_paused(state == PlayState::Paused);
    const std::string d = playback_->describe();
    if (!d.empty() && format_->text() != d) {
        format_->set_text(d);
        format_->set_visible(true);
    }

    // Twice a second: chapter name, chapter marks, "Ends at", the speed actually playing (an engine
    // may refuse one: the TV decoder above 60 fps of video).
    clock_s_ += dt;
    speed_settle_s_ -= dt;
    history_s_ += dt;
    if (history_s_ >= 10 && state == PlayState::Playing) {
        history_s_ = 0;
        remember_position();
    }
    if (clock_s_ >= 0.5f) {
        clock_s_ = 0;
        if (playback_->can_change_rate() && applied_ && speed_settle_s_ <= 0) {
            const float r = playback_->rate();
            size_t best = 0;
            for (size_t i = 1; i < std::size(kSpeeds); ++i)
                if (std::abs(kSpeeds[i] - r) < std::abs(kSpeeds[best] - r)) best = i;
            if (static_cast<int>(best) != settings_.speed) {
                ui.toast(std::string(kSpeedNames[static_cast<size_t>(settings_.speed)]) + " is too fast for this video here; playing at " +
                         kSpeedNames[best] + ".");
                settings_.speed = static_cast<int>(best);
            }
            speed_btn_->set_text(kSpeedNames[best]);
        }
        const auto chapters = playback_->chapters();
        const int c = playback_->chapter();
        chapter_->set_text(c >= 0 && c < static_cast<int>(chapters.size())
                               ? "Chapter " + std::to_string(c + 1) + " · " + chapters[static_cast<size_t>(c)].name
                               : std::string());
        seek_->marks.clear();
        if (len > 0)
            for (const auto &ch : chapters) seek_->marks.push_back(static_cast<float>(ch.start_ms) / len);
        if (len > 0) {
            const float rate = playback_->can_change_rate() ? std::max(0.1f, kSpeeds[settings_.speed]) : 1.0f;  // settled above
            const std::time_t end = std::time(nullptr) + static_cast<std::time_t>((len - t) / 1000 / rate);
            char buf[32];
            std::strftime(buf, sizeof buf, "Ends at %H:%M", std::localtime(&end));
            std::string s = buf;
            if (settings_.sleep_left_s > 0)
                s += " · sleep in " + clock_text(static_cast<int64_t>(settings_.sleep_left_s * 1000));
            ends_->set_text(s);
        }
    }

    const bool waiting = state == PlayState::Opening || (!shown && d.empty());
    spinner_->set_visible(waiting);
    if (!reported_ && shown) {
        reported_ = true;
        WNK_LOGI(kCat, "first frame shown (%s)", d.c_str());
    }

    // Auto-hide while playing video; audio-only, paused and an open panel keep the controls.
    if (controls_ && !panel_->visible() && state == PlayState::Playing && shown) {
        idle_s_ += dt;
        if (idle_s_ >= kHideAfterS) hide_controls(ui);
    }
    const float speed = dt * 5;  // ~200 ms fades
    overlay_alpha_ += std::clamp(overlay_target_ - overlay_alpha_, -speed, speed);
    overlay_->set_opacity(overlay_alpha_);
}

}  // namespace vplayer
