// SPDX-License-Identifier: GPL-3.0-or-later
//
// VPlayer: libVLC on LG webOS, on the webos-native-app-kit app shell.
// Design: ../docs/RESEARCH.md. Plan: ../docs/ROADMAP.md.
//
// Today this screen proves the libVLC runtime inside the package: it starts libVLC on a worker and
// shows its version, how long the plugin scan took, and which audio outputs could be loaded.
#include "vlc_engine.hpp"

#include "wnk/app/app.hpp"
#include "wnk/async/async.hpp"
#include "wnk/log.hpp"
#include "wnk/ui/navigation.hpp"
#include "wnk/ui/widgets.hpp"

#include <memory>
#include <string>

namespace {

constexpr const char *kCat = "vplayer";

struct EngineResult {
    std::shared_ptr<vplayer::VlcEngine> engine;
    vplayer::VlcInfo info;
};

std::string join(const std::vector<std::string> &v) {
    std::string out;
    for (const auto &s : v) out += (out.empty() ? "" : ", ") + s;
    return out.empty() ? "none" : out;
}

class HomeScreen : public wnk::ui::Screen {
 public:
    HomeScreen() : Screen("home") {
        auto &col = add<wnk::ui::Column>(28);
        col.set_padding(wnk::ui::Insets::all(96));
        col.add<wnk::ui::Label>("VPlayer", wnk::ui::TextRole::Title);
        status_ = &col.add<wnk::ui::Label>("Starting libVLC…");
        details_ = &col.add<wnk::ui::Label>("");
        details_->set_muted(true);
        auto &buttons = col.add<wnk::ui::Row>(24);
        buttons.add<wnk::ui::Button>("About", [](wnk::ui::Ui &ui) {
            ui.toast("VPlayer is powered by libVLC (LGPL). Not affiliated with VideoLAN.");
        });
    }

    void show_engine(const vplayer::VlcInfo &info) {
        if (info.available) {
            status_->set_text("libVLC " + info.version + " is running.");
            details_->set_text("Started in " + std::to_string(info.init_time.count()) + " ms. Audio outputs: " +
                               join(info.audio_outputs) + ". Built with " + info.compiler + ".");
        } else {
            status_->set_text("libVLC is not available.");
            details_->set_text(info.error);
        }
    }

 private:
    wnk::ui::Label *status_ = nullptr;
    wnk::ui::Label *details_ = nullptr;
};

class App : public wnk::app::Delegate {
 public:
    void on_start(wnk::app::Context &ctx) override {
        auto &ui = ctx.ui();
        nav_ = &ui.root().add<wnk::ui::Navigator>();
        auto home = std::make_unique<HomeScreen>();
        home_ = home.get();
        nav_->push(ui, std::move(home));

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
                if (home_) home_->show_engine(r.info);
            });
        WNK_LOGI(kCat, "started");
    }

    void on_stop(wnk::app::Context &) override {
        cancel_.cancel();
        home_ = nullptr;
        engine_.reset();
    }

 private:
    wnk::async::Lifetime lifetime_;
    wnk::async::CancellationSource cancel_;
    wnk::ui::Navigator *nav_ = nullptr;
    HomeScreen *home_ = nullptr;
    std::shared_ptr<vplayer::VlcEngine> engine_;
};

}  // namespace

int main(int argc, char **argv) {
    wnk::app::AppConfig config;
    config.fallback_app_id = "com.sostk.vplayer";  // used when $APPID is unset (host runs)
    config.title = "VPlayer";
    config.script_env_prefix = "APP_VPLAYER";  // APP_VPLAYER_KEYS=Return,Escape drives the host smoke test
    return wnk::app::run(argc, argv, config, [] { return std::make_unique<App>(); });
}
