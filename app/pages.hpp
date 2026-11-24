// SPDX-License-Identifier: GPL-3.0-or-later
//
// The pages under the top bar's tabs. Each is a plain View the main screen shows one at a time.
#pragma once

#include "app_settings.hpp"
#include "history.hpp"
#include "playlist.hpp"
#include "ui/theme.hpp"
#include "vlc_engine.hpp"

#include "wnk/app/app.hpp"
#include "wnk/async/async.hpp"
#include "wnk/platform/files.hpp"
#include "wnk/ui/collection.hpp"

#include <chrono>
#include <functional>
#include <string>
#include <vector>

namespace vplayer {

enum class Tab : int { Video = 0, Music, Browse, Network, Playlists, Count };

// What pages need from the app: the shell's services and the main screen's navigation.
struct Services {
    wnk::app::Context *ctx = nullptr;
    PlayHistory *history = nullptr;  // resume points, "Continue watching" (main thread)
    const AppSettings *settings = nullptr;
    // The viewer's own playlists (playlist.hpp), and saving them after a change (main thread).
    std::vector<MyPlaylist> *my_playlists = nullptr;
    std::function<void()> save_my_playlists;
    class Thumbnails *thumbnails = nullptr;  // video thumbnails, once libVLC runs (thumbnails.hpp)
    libvlc_instance_t *vlc = nullptr;        // once libVLC runs
    std::chrono::steady_clock::time_point login_cancelled{};  // the viewer last cancelled a sign-in
    std::function<void(Tab)> show_tab;
    std::function<void(const std::string &folder)> browse_to;  // Browse tab, at that folder
    std::function<void(const wnk::platform::FileEntry &)> play;
    // Opens the player on queue[index], with the queue for previous/next (network folders: the
    // entries' paths are libVLC addresses).
    std::function<void(std::vector<wnk::platform::FileEntry> queue, size_t index)> play_queue;
    std::function<void()> open_stream;  // the Network tab's "Open a network stream" dialog
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
    void show_rail(wnk::ui::Ui &ui);  // the selected list: continue watching, or recently added

    Services &s_;
    wnk::async::Lifetime lifetime_;
    std::vector<wnk::platform::RemovableVolume> volumes_;
    std::vector<wnk::platform::FileEntry> recent_;
    std::vector<HistoryEntry> continue_;
    uint64_t history_gen_ = ~uint64_t{0};
    int rail_mode_ = 1;           // 0 continue watching, 1 recently added
    bool continue_shown_ = false;  // switched to "Continue watching" once it had entries
    bool recent_loaded_ = false;
    uint64_t thumbs_gen_ = 0;
    float poll_ = 0;
    bool first_ = true;
    ui::PillTabs *rail_tabs_ = nullptr;
    ui::HeroCard *hero_ = nullptr;
    wnk::ui::Label *recent_source_ = nullptr;
    wnk::ui::Label *recent_empty_ = nullptr;
    wnk::ui::CollectionView *rail_ = nullptr;
};

// Browse tab: drives, then folders and files in a grid. Back goes up a folder.
class BrowsePage : public Page {
 public:
    explicit BrowsePage(Services &services);
    // Shows `folder` ("" for the drives), then focuses the entry at `focus_path` if given.
    void open(wnk::ui::Ui &ui, const std::string &folder, const std::string &focus_path = {}, bool take_focus = false);
    bool on_action(wnk::ui::Ui &ui, const wnk::input::ActionEvent &e) override;
    void update(wnk::ui::Ui &ui, float dt) override;
    void on_shown(wnk::ui::Ui &ui) override;

 private:
    void show_volumes(wnk::ui::Ui &ui, const std::string &focus_path = {});
    void show_entries(wnk::ui::Ui &ui, std::vector<wnk::platform::FileEntry> entries, const std::string &error,
                      const std::string &focus_path);
    void place_focus(wnk::ui::Ui &ui, const std::string &focus_path);
    void set_crumbs(const std::string &folder);
    const wnk::platform::RemovableVolume *volume_of(const std::string &path) const;
    void activate(wnk::ui::Ui &ui, size_t index);

    Services &s_;
    wnk::async::Lifetime lifetime_;
    wnk::async::CancellationSource listing_;
    std::vector<wnk::platform::RemovableVolume> volumes_;
    std::vector<wnk::platform::FileEntry> entries_;
    std::string folder_;  // empty: the list of drives
    float poll_ = 0;
    bool started_ = false;
    bool want_focus_ = false;  // move focus into the grid when the listing arrives (browse_to)
    uint64_t thumbs_gen_ = 0;
    wnk::ui::Row *crumbs_ = nullptr;
    wnk::ui::Label *count_ = nullptr;
    wnk::ui::Label *message_ = nullptr;
    wnk::ui::CollectionView *grid_ = nullptr;
};

// A tab whose feature has not been built yet: says what it will do.
class PlaceholderPage : public Page {
 public:
    PlaceholderPage(const std::string &icon_url, const std::string &title, const std::string &body);
};

}  // namespace vplayer
