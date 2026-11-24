// SPDX-License-Identifier: GPL-3.0-or-later
//
// The pages under the top bar's tabs. Each is a plain View the main screen shows one at a time.
#pragma once

#include "ui/theme.hpp"
#include "vlc_engine.hpp"

#include "wnk/app/app.hpp"
#include "wnk/async/async.hpp"
#include "wnk/platform/files.hpp"
#include "wnk/ui/collection.hpp"

#include <functional>
#include <string>
#include <vector>

namespace vplayer {

enum class Tab : int { Video = 0, Music, Browse, Network, Playlists, Count };

// What pages need from the app: the shell's services and the main screen's navigation.
struct Services {
    wnk::app::Context *ctx = nullptr;
    std::function<void(Tab)> show_tab;
    std::function<void(const std::string &folder)> browse_to;  // Browse tab, at that folder
    std::function<void(const wnk::platform::FileEntry &)> play;
};

class Page : public wnk::ui::View {
 public:
    // The tab became visible.
    virtual void on_shown(wnk::ui::Ui &) {}
};

// Video tab: the USB drive (or how to add media) as a hero card, shortcuts, and the newest videos.
class HomePage : public Page {
 public:
    explicit HomePage(Services &services);
    void update(wnk::ui::Ui &ui, float dt) override;

 private:
    void refresh_volumes(wnk::ui::Ui &ui, bool force);
    void show_recent(wnk::ui::Ui &ui, std::vector<wnk::platform::FileEntry> files);

    Services &s_;
    wnk::async::Lifetime lifetime_;
    std::vector<wnk::platform::RemovableVolume> volumes_;
    std::vector<wnk::platform::FileEntry> recent_;
    float poll_ = 0;
    bool first_ = true;
    ui::HeroCard *hero_ = nullptr;
    wnk::ui::Label *recent_source_ = nullptr;
    wnk::ui::Label *recent_empty_ = nullptr;
    wnk::ui::CollectionView *rail_ = nullptr;
};

// A tab whose feature has not been built yet: says what it will do.
class PlaceholderPage : public Page {
 public:
    PlaceholderPage(const std::string &icon_url, const std::string &title, const std::string &body);
};

}  // namespace vplayer
