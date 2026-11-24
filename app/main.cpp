// SPDX-License-Identifier: GPL-3.0-or-later
//
// VPlayer: made with the kit's tools/new-app.py. The kit's docs/AGENT-APP-GUIDE.md maps features
// onto its API (lists and grids, HTTP, storage, playback); its applications/examples/ shows each of
// them in a working app.
#include "wnk/app/app.hpp"
#include "wnk/log.hpp"
#include "wnk/ui/navigation.hpp"
#include "wnk/ui/widgets.hpp"

#include <memory>
#include <string>

namespace {

constexpr const char *kCat = "vplayer";

class HomeScreen : public wnk::ui::Screen {
 public:
    HomeScreen() : Screen("home") {
        auto &col = add<wnk::ui::Column>(32);
        col.set_padding(wnk::ui::Insets::all(96));
        col.add<wnk::ui::Label>("VPlayer", wnk::ui::TextRole::Title);
        auto &status = col.add<wnk::ui::Label>("Press OK on a button. Back twice quits.");
        status.set_muted(true);
        auto &buttons = col.add<wnk::ui::Row>(24);
        buttons.add<wnk::ui::Button>("Say hello", [this, &status](wnk::ui::Ui &) {
            ++presses_;
            WNK_LOGI(kCat, "hello pressed %d", presses_);
            status.set_text("Hello from the TV! Pressed " + std::to_string(presses_) + (presses_ == 1 ? " time." : " times."));
        });
        buttons.add<wnk::ui::Button>("Show a toast", [](wnk::ui::Ui &ui) { ui.toast("A toast from VPlayer"); });
    }

 private:
    int presses_ = 0;
};

class App : public wnk::app::Delegate {
 public:
    void on_start(wnk::app::Context &ctx) override {
        auto &ui = ctx.ui();
        nav_ = &ui.root().add<wnk::ui::Navigator>();
        nav_->push(ui, std::make_unique<HomeScreen>());
        WNK_LOGI(kCat, "started");
    }

 private:
    wnk::ui::Navigator *nav_ = nullptr;
};

}  // namespace

int main(int argc, char **argv) {
    wnk::app::AppConfig config;
    config.fallback_app_id = "com.sostk.vplayer";  // used when $APPID is unset (host runs)
    config.title = "VPlayer";
    config.script_env_prefix = "APP_VPLAYER";  // APP_VPLAYER_KEYS=Return,Escape drives the host smoke test
    return wnk::app::run(argc, argv, config, [] { return std::make_unique<App>(); });
}
