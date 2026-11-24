// SPDX-License-Identifier: GPL-3.0-or-later
//
// Playlists tab: M3U/M3U8, PLS and XSPF playlists and IPTV channel lists.
//
//   root      "Add a playlist or IPTV list" by address, the lists added that way (kept), and the
//             playlist files found on the drives
//   a list    read by the app (playlist.hpp): local files directly, addresses over HTTP(S) through
//             the kit's transport; an HLS address is a stream and simply plays. A list with several
//             groups (IPTV group-title) shows its groups first.
//   entries   tiles with the channel's logo when given; an entry plays with the others of its group
//             as the queue
//
// Back goes up (entries → groups → lists). The red key on an added list forgets it.
#pragma once

#include "pages.hpp"
#include "playlist.hpp"

#include <string>
#include <vector>

namespace vplayer {

class PlaylistsPage : public Page {
 public:
    explicit PlaylistsPage(Services &services);
    void on_shown(wnk::ui::Ui &ui) override;
    bool on_action(wnk::ui::Ui &ui, const wnk::input::ActionEvent &e) override;
    void open_address_dialog(wnk::ui::Ui &ui);

 private:
    enum class Level : uint8_t { Root, Groups, Entries };
    struct RootItem {
        enum class Kind : uint8_t { Add, Mine, Saved, File } kind;
        std::string name, location;
    };

    void show_root(wnk::ui::Ui &ui, bool focus);
    void open_list(wnk::ui::Ui &ui, const std::string &name, const std::string &location);
    void show_list(wnk::ui::Ui &ui, Playlist list, const std::string &error);
    void show_groups(wnk::ui::Ui &ui, size_t focus);
    void show_entries(wnk::ui::Ui &ui, const std::string &group);
    void activate(wnk::ui::Ui &ui, size_t i);
    void save_lists();
    void set_header(const std::string &crumb2, const std::string &crumb3, const std::string &count);

    Services &s_;
    wnk::async::Lifetime lifetime_;
    wnk::async::CancellationSource loading_;
    std::vector<SavedPlaylist> saved_;
    bool saved_loaded_ = false;
    std::vector<wnk::platform::FileEntry> files_;  // playlists on the drives
    std::vector<RootItem> root_;
    Level level_ = Level::Root;
    std::string list_name_, list_location_;
    int mine_ = -1;  // the own playlist shown, -1 when another list
    Playlist list_;
    std::vector<std::string> groups_;
    std::string group_;                     // the group shown (Entries)
    std::vector<const PlaylistItem *> shown_;  // entries shown
    wnk::ui::Row *crumbs_ = nullptr;
    wnk::ui::Label *count_ = nullptr;
    wnk::ui::Label *message_ = nullptr;
    wnk::ui::Label *hint_ = nullptr;
    wnk::ui::CollectionView *grid_ = nullptr;
};

}  // namespace vplayer
