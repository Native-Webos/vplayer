// SPDX-License-Identifier: GPL-3.0-or-later
//
// VPlayer: libVLC on LG webOS, on the webos-native-app-kit app shell.
// Design: ../docs/RESEARCH.md. Plan: ../docs/ROADMAP.md. Look: the approved mockups
// (claude.ai/artifact/G2ZZQ1qBnYBgtGmEU6NAQ6), built from ui/theme.hpp.
//
// The main screen is a top bar (logo, tabs, search, settings) over one page per tab. libVLC starts
// on a worker while the UI comes up; Settings shows what it reported.
#include "library.hpp"
#include "pages.hpp"
#include "ui/theme.hpp"
#include "vlc_engine.hpp"

#include "wnk/app/app.hpp"
#include "wnk/async/async.hpp"
#include "wnk/log.hpp"
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
        pages_[static_cast<size_t>(Tab::Network)] = &stack.add<vplayer::PlaceholderPage>(
            vui::icon("server"), "Network shares are coming",
            "SMB, NFS, SFTP and FTP shares and DLNA media servers on your network, found automatically.");
        pages_[static_cast<size_t>(Tab::Playlists)] = &stack.add<vplayer::PlaceholderPage>(
            vui::icon("playlist"), "Playlists are coming", "M3U, XSPF and PLS playlists and IPTV channel lists.");
        for (size_t i = 0; i < pages_.size(); ++i) pages_[i]->set_visible(i == 0);
    }

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
    Tab current_ = Tab::Video;
};

class App : public wnk::app::Delegate {
 public:
    void on_start(wnk::app::Context &ctx) override {
        vui::set_icon_dir(ctx.launch().app_dir + "/icons");
        auto &ui = ctx.ui();

        services_.ctx = &ctx;
        services_.show_tab = [this, &ctx](Tab t) { main_->go(ctx.ui(), t); };
        services_.browse_to = [this, &ctx](const std::string &folder) { main_->browse_to(ctx.ui(), folder); };
        services_.play = [&ctx](const wnk::platform::FileEntry &e) {
            WNK_LOGI(kCat, "play requested: %s", e.path.c_str());
            ctx.ui().toast("Playback is the next step: \"" + vplayer::display_name(e.name) + "\"");
        };

        nav_ = &ui.root().add<Navigator>();
        auto main = std::make_unique<MainScreen>(services_, [this](Ui &u) { show_about(u); });
        main_ = main.get();
        nav_->push(ui, std::move(main));
        main_->focus_page(ui);  // start on the hero card, not the first thing in the top bar
        apply_launch_params(ctx, ctx.launch().params_json);

        const std::string app_dir = ctx.launch().app_dir;
        wnk::async::run_then_post<EngineResult>(
            ctx.pool(), ctx.dispatcher(), lifetime_.weak(), cancel_.token(),
            [app_dir](const wnk::async::CancellationToken &) {
                EngineResult r;
                r.engine = vplayer::VlcEngine::create(app_dir, r.info);
                return r;
            },
            [this](EngineResult r) {
                engine_ = std::move(r.engine);
                vlc_info_ = std::move(r.info);
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
    // Launch parameters (SAM's JSON): {"browse": "/tmp/usb/sda/sda1/Movies"} opens Browse there.
    void apply_launch_params(wnk::app::Context &ctx, const std::string &params_json) {
        if (params_json.empty()) return;
        const auto j = nlohmann::json::parse(params_json, nullptr, false);
        if (!j.is_object()) return;
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
    std::optional<vplayer::VlcInfo> vlc_info_;
};

}  // namespace

int main(int argc, char **argv) {
    wnk::app::AppConfig config;
    config.fallback_app_id = "com.sostk.vplayer";  // used when $APPID is unset (host runs)
    config.title = "VPlayer";
    config.script_env_prefix = "APP_VPLAYER";  // APP_VPLAYER_KEYS=Return,Escape drives the host smoke test
    config.theme = vplayer::ui::make_theme();
    config.font_regular = "fonts/Inter-Regular.ttf";
    config.font_bold = "fonts/Inter-SemiBold.ttf";
    return wnk::app::run(argc, argv, config, [] { return std::make_unique<App>(); });
}
