// SPDX-License-Identifier: GPL-3.0-or-later
//
// VPlayer: libVLC on LG webOS, on the webos-native-app-kit app shell.
// Design: ../docs/RESEARCH.md. Plan: ../docs/ROADMAP.md. Look: the approved mockups
// (claude.ai/artifact/G2ZZQ1qBnYBgtGmEU6NAQ6), built from ui/theme.hpp.
//
// The main screen is a top bar (logo, tabs, search, settings) over one page per tab. libVLC starts
// on a worker while the UI comes up; Settings shows what it reported.
#include "library.hpp"
#include "network_page.hpp"
#include "pages.hpp"
#include "player/hw_playback.hpp"
#include "player/player_screen.hpp"
#include "ui/theme.hpp"
#include "vlc_dialogs.hpp"
#include "vlc_engine.hpp"

#include "wnk/app/app.hpp"
#include "wnk/async/async.hpp"
#include "wnk/log.hpp"
#include "wnk/media/device_caps.hpp"
#include "wnk/ui/navigation.hpp"
#include "wnk/ui/widgets.hpp"

#include <nlohmann/json.hpp>

#include <array>
#include <memory>
#include <optional>
#include <string>

namespace {

using namespace wnk::ui;
using vplayer::Tab;
namespace vui = vplayer::ui;
namespace input = wnk::input;

constexpr const char *kCat = "vplayer";

struct EngineResult {
    std::shared_ptr<vplayer::VlcEngine> engine;
    vplayer::VlcInfo info;
    vplayer::HwOptions hw;   // what the TV says it supports (Dolby Vision, Atmos)
    bool hw_available = false;
    std::string hw_why;
    std::vector<vplayer::HistoryEntry> history;  // read from the store on the same worker
};

class MainScreen : public Screen {
 public:
    MainScreen(vplayer::Services &services, std::function<void(Ui &)> on_settings)
        : Screen("main"), s_(services) {
        auto &col = add<Column>(40);
        col.set_padding(Insets{vui::metric::screen_x, vui::metric::screen_y, vui::metric::screen_x, vui::metric::screen_y});

        // ---- top bar ----
        auto &bar = col.add<Row>(40);
        bar.set_height(Dim::fixed(vui::metric::top_bar_h));
        auto &brand = bar.add<Row>(14);
        brand.set_align(Align::Start, Align::Center);
        auto &mark = brand.add<View>();
        mark.set_size(44, 44);
        mark.set_background(vui::col::accent, 14);
        mark.set_align(Align::Start, Align::Center);
        auto &play = mark.add<ImageView>(vui::icon("logo-play"));
        play.set_size(22, 22);
        play.set_fit(ImageFit::Contain);
        play.set_align(Align::Center, Align::Center);
        auto &word = brand.add<Label>("VPlayer");
        word.set_font_size(28);
        word.set_bold(true);
        word.set_align(Align::Start, Align::Center);

        tabs_ = &bar.add<vui::PillTabs>(std::vector<std::string>{"Video", "Music", "Browse", "Network", "Playlists"},
                                        [this](Ui &ui, int i) { show(ui, static_cast<Tab>(i)); });
        tabs_->set_align(Align::Start, Align::Center);
        bar.add<View>().set_width(Dim::fill());
        auto &actions = bar.add<Row>(16);
        actions.set_align(Align::End, Align::Center);
        actions.add<vui::IconButton>(vui::icon("search"), "Search",
                                     [](Ui &ui) { ui.toast("Search is coming soon."); }).name = "top.search";
        actions.add<vui::IconButton>(vui::icon("settings"), "Settings", std::move(on_settings)).name = "top.settings";

        // ---- pages ----
        auto &stack = col.add<View>();
        stack.set_height(Dim::fill());
        pages_[static_cast<size_t>(Tab::Video)] = &stack.add<vplayer::HomePage>(s_);
        pages_[static_cast<size_t>(Tab::Music)] = &stack.add<vplayer::PlaceholderPage>(
            vui::icon("music"), "Music is coming",
            "Albums, artists and songs from your drives and network shares, with VLC's audio formats.");
        browse_ = &stack.add<vplayer::BrowsePage>(s_);
        pages_[static_cast<size_t>(Tab::Browse)] = browse_;
        network_ = &stack.add<vplayer::NetworkPage>(s_, nullptr);
        pages_[static_cast<size_t>(Tab::Network)] = network_;
        pages_[static_cast<size_t>(Tab::Playlists)] = &stack.add<vplayer::PlaceholderPage>(
            vui::icon("playlist"), "Playlists are coming", "M3U, XSPF and PLS playlists and IPTV channel lists.");
        for (size_t i = 0; i < pages_.size(); ++i) pages_[i]->set_visible(i == 0);
    }

    vplayer::NetworkPage &network() { return *network_; }

    // Shows a tab's page (and selects the tab) without moving focus.
    void show(Ui &ui, Tab tab) {
        const auto index = static_cast<size_t>(tab);
        if (index >= pages_.size()) return;
        tabs_->set_selected(static_cast<int>(tab));
        for (size_t i = 0; i < pages_.size(); ++i) pages_[i]->set_visible(i == index);
        current_ = tab;
        pages_[index]->on_shown(ui);
    }

    // Focus inside the current page (its first focusable, or what it focused last). The tab is
    // focused first for an instant: the tab bar is a focus scope that returns to the tab it last
    // held, and that has to be the selected one, or Up from the page would land on another tab
    // (and moving onto a tab selects it).
    void focus_page(Ui &ui) {
        ui.focus(&tabs_->tab(static_cast<int>(current_)));
        ui.focus_within(pages_[static_cast<size_t>(current_)]);
    }

    // Shows a tab and moves focus into its page.
    void go(Ui &ui, Tab tab) {
        show(ui, tab);
        focus_page(ui);
    }

    void browse_to(Ui &ui, const std::string &folder) {
        show(ui, Tab::Browse);
        ui.focus(&tabs_->tab(static_cast<int>(Tab::Browse)));  // until the listing arrives and takes it
        browse_->open(ui, folder, {}, true);
    }

    bool on_action(Ui &ui, const input::ActionEvent &e) override {
        // Back that no page used: from another tab, return to Video first; from Video, the shell's
        // "press Back again to exit".
        if (e.action == input::Action::Back && current_ != Tab::Video) {
            if (e.phase == input::Phase::Press) {
                show(ui, Tab::Video);
                ui.focus(&tabs_->tab(0));
            }
            return true;
        }
        return Screen::on_action(ui, e);
    }

 private:
    vplayer::Services &s_;
    vui::PillTabs *tabs_ = nullptr;
    std::array<vplayer::Page *, static_cast<size_t>(Tab::Count)> pages_{};
    vplayer::BrowsePage *browse_ = nullptr;
    vplayer::NetworkPage *network_ = nullptr;
    Tab current_ = Tab::Video;
};

class App : public wnk::app::Delegate {
 public:
    void on_start(wnk::app::Context &ctx) override {
        vui::set_icon_dir(ctx.launch().app_dir + "/icons");
        auto &ui = ctx.ui();

        services_.ctx = &ctx;
        services_.history = &history_;
        services_.show_tab = [this, &ctx](Tab t) { main_->go(ctx.ui(), t); };
        services_.browse_to = [this, &ctx](const std::string &folder) { main_->browse_to(ctx.ui(), folder); };
        services_.play = [this, &ctx](const wnk::platform::FileEntry &e) { play(ctx.ui(), e); };
        services_.play_queue = [this, &ctx](std::vector<wnk::platform::FileEntry> queue, size_t index) {
            open_player(ctx.ui(), std::move(queue), index);
        };

        nav_ = &ui.root().add<Navigator>();
        auto main = std::make_unique<MainScreen>(services_, [this](Ui &u) { show_about(u); });
        main_ = main.get();
        nav_->push(ui, std::move(main));
        main_->focus_page(ui);  // start on the hero card, not the first thing in the top bar
        apply_launch_params(ctx, ctx.launch().params_json);

        const std::string app_dir = ctx.launch().app_dir;
        auto *store = ctx.store();
        wnk::async::run_then_post<EngineResult>(
            ctx.pool(), ctx.dispatcher(), lifetime_.weak(), cancel_.token(),
            [app_dir, store](const wnk::async::CancellationToken &) {
                EngineResult r;
                r.history = vplayer::read_history(store);  // blocking store read: here, on a worker
                r.engine = vplayer::VlcEngine::create(app_dir, r.info);
                // The TV's decoders and what it may be told about (blocking Luna calls: here, on a worker).
                r.hw_available = vplayer::hw_playback_available(&r.hw_why);
                if (r.hw_available) {
                    const auto caps = wnk::media::DeviceMediaCaps::detect();
                    r.hw.dolby_vision = caps.dolby_vision == true;
                    r.hw.atmos = caps.dolby_atmos == true;
                }
                return r;
            },
            [this, &ctx](EngineResult r) {
                engine_ = std::move(r.engine);
                vlc_info_ = std::move(r.info);
                history_.adopt(ctx.store(), std::move(r.history));
                hw_ = r.hw;
                hw_available_ = r.hw_available;
                if (engine_) {
                    // libVLC's sign-in and certificate questions, and the network page's discovery.
                    dialogs_ = std::make_unique<vplayer::VlcDialogs>(engine_->instance(), ctx.dispatcher(),
                                                                     [&ctx]() -> Ui & { return ctx.ui(); });
                    main_->network().set_vlc(engine_->instance());
                }
                WNK_LOGI(kCat, "hardware decoding: %s%s (Dolby Vision %s, Atmos %s)", r.hw_available ? "yes" : "no",
                         r.hw_available ? "" : (", " + r.hw_why).c_str(), r.hw.dolby_vision ? "yes" : "no",
                         r.hw.atmos ? "yes" : "no");
                if (pending_play_) {  // a {"play": ...} launch waited for the engine
                    auto e = *pending_play_;
                    pending_play_.reset();
                    play(ctx.ui(), e);
                }
            });
        WNK_LOGI(kCat, "started");
    }

    void on_relaunch(wnk::app::Context &ctx, const std::string &params_json) override {
        apply_launch_params(ctx, params_json);
    }

    void on_stop(wnk::app::Context &) override {
        cancel_.cancel();
        engine_.reset();
    }

 private:
    // A player for one file: on the TV the hardware path (libVLC demuxes, the TV decodes) for video
    // unless `software` or the "decoder" launch parameter says otherwise; libVLC's own decoders for
    // the rest; the demo engine in host builds without libVLC.
    std::unique_ptr<vplayer::Playback> open_playback(const wnk::platform::FileEntry &e, bool software) {
        const bool audio_only = e.kind == wnk::platform::FileKind::Audio;
        const bool hw = engine_ && hw_available_ && !software && !audio_only && decoder_ != "software";
        if (hw) {
            WNK_LOGI(kCat, "engine: hardware (TV decoder) for %s", e.name.c_str());
            return vplayer::open_hw_playback(engine_->instance(), e.path, services_.ctx->dispatcher(), hw_, vlc_options_);
        }
        if (engine_) {
            WNK_LOGI(kCat, "engine: software (libVLC) for %s", e.name.c_str());
            return vplayer::open_vlc_playback(engine_->instance(), e.path, vlc_options_);
        }
        if (vlc_info_ && !vlc_info_->available) return vplayer::open_demo_playback();
        return nullptr;
    }

    // Opens the player on `e`, with the playable files of its folder as the queue (previous/next,
    // play the next file automatically).
    void play(Ui &ui, const wnk::platform::FileEntry &e) {
        WNK_LOGI(kCat, "play: %s", e.path.c_str());
        if (!engine_ && !vlc_info_) {
            ui.toast("The player is still starting. Try again in a moment.");
            return;
        }
        std::vector<wnk::platform::FileEntry> queue;
        size_t index = 0;
        std::vector<wnk::platform::FileEntry> entries;
        std::string err;
        const std::string folder = e.path.substr(0, e.path.rfind('/'));
        if (wnk::platform::list_folder(folder, &entries, &err, 5000)) {
            for (auto &x : entries) {
                const bool disc = x.kind == wnk::platform::FileKind::Other &&
                                  (vplayer::extension_upper(x.name) == "ISO" || vplayer::extension_upper(x.name) == "IMG");
                if (x.kind != wnk::platform::FileKind::Video && x.kind != wnk::platform::FileKind::Audio && !disc) continue;
                if (x.path == e.path) index = queue.size();
                queue.push_back(std::move(x));
            }
        }
        if (queue.empty() || queue[index].path != e.path) {
            queue.assign(1, e);
            index = 0;
        }
        open_player(ui, std::move(queue), index);
    }

    void open_player(Ui &ui, std::vector<wnk::platform::FileEntry> queue, size_t index) {
        if (queue.empty()) return;
        if (!engine_ && !vlc_info_) {
            ui.toast("The player is still starting. Try again in a moment.");
            return;
        }
        // A player already open (a relaunch with "play", or a file opened from elsewhere) is replaced.
        if (auto *open_player = dynamic_cast<vplayer::PlayerScreen *>(nav_->top())) open_player->close(ui);
        auto player = std::make_unique<vplayer::PlayerScreen>(
            services_, *nav_, [this](const wnk::platform::FileEntry &f, bool sw) { return open_playback(f, sw); }, std::move(queue),
            index);
        if (pending_panel_ >= 0) player->open_panel_when_playing(pending_panel_);
        if (pending_at_ms_ >= 0) player->start_at(pending_at_ms_);
        pending_panel_ = -1;
        pending_at_ms_ = -1;
        nav_->push(ui, std::move(player));
    }

    // Launch parameters (SAM's JSON): {"browse": "/tmp/usb/sda/sda1/Movies"} opens Browse there.
    void apply_launch_params(wnk::app::Context &ctx, const std::string &params_json) {
        if (params_json.empty()) return;
        const auto j = nlohmann::json::parse(params_json, nullptr, false);
        if (!j.is_object()) return;
        // {"vlcOptions": [":no-audio", ...]}: per-media libVLC options for the next plays (debugging).
        vlc_options_.clear();
        if (auto it = j.find("vlcOptions"); it != j.end() && it->is_array()) {
            for (const auto &o : *it)
                if (o.is_string()) vlc_options_.push_back(o.get<std::string>());
        }
        // {"at": seconds}: start the "play" file there.
        if (auto it = j.find("at"); it != j.end() && it->is_number()) pending_at_ms_ = static_cast<int64_t>(it->get<double>() * 1000);
        // {"decoder": "software" | "hardware"}: force an engine for the next plays (testing).
        if (auto it = j.find("decoder"); it != j.end() && it->is_string()) decoder_ = it->get<std::string>();
        // {"panel": "audio" | "subtitles" | "video" | "playback" | "info"}: open the player's panel on
        // that tab once the file plays (with "play"; for checking the panel on a TV without a remote).
        if (auto it = j.find("panel"); it != j.end() && it->is_string()) {
            const std::string t = it->get<std::string>();
            const char *tabs[] = {"audio", "subtitles", "video", "playback", "info"};
            for (int i = 0; i < 5; ++i)
                if (t == tabs[i]) pending_panel_ = i;
        }
        if (auto it = j.find("play"); it != j.end() && it->is_string()) {
            // {"play": "/tmp/usb/sda/sda1/Film.mkv"}: straight into the player (also for testing).
            wnk::platform::FileEntry e;
            e.path = it->get<std::string>();
            e.name = e.path.substr(e.path.rfind('/') + 1);
            if (vlc_info_) {
                play(ctx.ui(), e);  // the engine is up (a relaunch)
            } else {
                pending_play_ = e;  // played when the engine reports
            }
        }
        if (auto it = j.find("browse"); it != j.end() && it->is_string()) {
            WNK_LOGI(kCat, "launch: browse %s", it->get<std::string>().c_str());
            main_->browse_to(ctx.ui(), it->get<std::string>());
        }
    }

    void show_about(Ui &ui) {
        std::string msg;
        if (!vlc_info_) {
            msg = "libVLC is still starting.";
        } else if (vlc_info_->available) {
            msg = "Playback engine: libVLC " + vlc_info_->version + ", started in " +
                  std::to_string(vlc_info_->init_time.count()) + " ms.";
        } else {
            msg = "libVLC is not available: " + vlc_info_->error;
        }
        msg += "\nVPlayer uses libVLC (LGPL) and is not affiliated with VideoLAN.";
        auto d = std::make_unique<Dialog>("About VPlayer", msg);
        d->add_button("Close");
        ui.show_dialog(std::move(d));
    }

    wnk::async::Lifetime lifetime_;
    wnk::async::CancellationSource cancel_;
    vplayer::Services services_;
    Navigator *nav_ = nullptr;
    MainScreen *main_ = nullptr;
    std::shared_ptr<vplayer::VlcEngine> engine_;
    std::unique_ptr<vplayer::VlcDialogs> dialogs_;
    vplayer::PlayHistory history_;
    std::optional<vplayer::VlcInfo> vlc_info_;
    std::optional<wnk::platform::FileEntry> pending_play_;
    std::vector<std::string> vlc_options_;
    int pending_panel_ = -1;
    vplayer::HwOptions hw_;
    bool hw_available_ = false;
    std::string decoder_ = "auto";
    int64_t pending_at_ms_ = -1;
};

}  // namespace

int main(int argc, char **argv) {
    wnk::app::AppConfig config;
    config.fallback_app_id = "com.sostk.vplayer";  // used when $APPID is unset (host runs)
    config.title = "VPlayer";
    config.script_env_prefix = "APP_VPLAYER";  // APP_VPLAYER_KEYS=Return,Escape drives the host smoke test
    config.theme = vplayer::ui::make_theme();
    config.storage = true;  // saved servers and addresses, resume points (DB8 on the TV)
    config.font_regular = "fonts/Inter-Regular.ttf";
    config.font_bold = "fonts/Inter-SemiBold.ttf";
    return wnk::app::run(argc, argv, config, [] { return std::make_unique<App>(); });
}
