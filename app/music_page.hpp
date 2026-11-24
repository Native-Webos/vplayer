// SPDX-License-Identifier: GPL-3.0-or-later
//
// Music tab: the albums on the drives (music.hpp: a folder of audio files, its artwork when the
// folder has a cover image), "Shuffle all" first; an album shows its tracks, and a track plays with
// the rest of the album after it. Back returns to the albums. Drives are scanned on a worker when
// the tab is shown and when drives come or go.
#pragma once

#include "music.hpp"
#include "pages.hpp"

#include <vector>

namespace vplayer {

class MusicPage : public Page {
 public:
    explicit MusicPage(Services &services);
    void on_shown(wnk::ui::Ui &ui) override;
    void update(wnk::ui::Ui &ui, float dt) override;
    bool on_action(wnk::ui::Ui &ui, const wnk::input::ActionEvent &e) override;

 private:
    void scan();
    // The albums: all of them (artist < 0) or one artist's.
    void show_albums(wnk::ui::Ui &ui, size_t focus, int artist = -1);
    void show_artists(wnk::ui::Ui &ui, size_t focus);
    void add_crumb(wnk::ui::Ui &ui, const std::string &text, bool current);
    void show_album(wnk::ui::Ui &ui, size_t album);
    void show_tracks(wnk::ui::Ui &ui);
    void activate(wnk::ui::Ui &ui, size_t i);

    Services &s_;
    wnk::async::Lifetime lifetime_;
    std::vector<wnk::platform::RemovableVolume> volumes_;
    std::vector<Album> albums_;
    bool scanned_ = false, scanning_ = false;
    enum class Level : uint8_t { Albums, Artists, Album };
    Level view_ = Level::Albums;
    int album_ = -1;   // the album shown, -1: none
    int artist_ = -1;  // the artist whose albums show (or whose album), -1: all albums
    std::vector<Artist> artists_;
    std::vector<size_t> listed_;  // the album indexes the albums grid shows, after its first tiles
    size_t lead_ = 0;             // tiles before the albums (Shuffle all, Artists / Play all)
    std::vector<TrackTags> tags_;   // of the album shown, once read
    std::vector<size_t> order_;     // its tracks in tag order
    float poll_ = 0;
    wnk::ui::Row *crumbs_ = nullptr;
    wnk::ui::Label *count_ = nullptr;
    wnk::ui::Label *message_ = nullptr;
    wnk::ui::CollectionView *grid_ = nullptr;
};

}  // namespace vplayer
