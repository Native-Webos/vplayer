// SPDX-License-Identifier: GPL-3.0-or-later
//
// Search across what VPlayer knows: video, audio and playlist files on the drives (an index built
// on a worker when the search opens) and the play history (network files included). A result
// matches when every word of the query appears in its name, ignoring case, accents and the
// separators file names use ("The.Movie_2024" matches "movie 2024").
#pragma once

#include "pages.hpp"

#include "wnk/ui/navigation.hpp"

#include <string>
#include <vector>

namespace vplayer {

// Lower case, Latin accents removed (é → e), '.', '_' and '-' as spaces.
std::string search_fold(const std::string &utf8);
// Every word of `query` in `name` (both folded); an empty query matches nothing.
bool search_match(const std::string &name, const std::string &query);

class SearchScreen : public wnk::ui::Screen {
 public:
    explicit SearchScreen(Services &services);
    void on_show(wnk::ui::Ui &ui) override;

 private:
    void index();
    void refresh(wnk::ui::Ui &ui);
    void activate(wnk::ui::Ui &ui, size_t i);

    Services &s_;
    wnk::async::Lifetime lifetime_;
    std::vector<wnk::platform::FileEntry> all_;      // drives and history
    std::vector<wnk::platform::FileEntry> results_;
    bool indexed_ = false;
    std::string query_;
    wnk::ui::TextField *field_ = nullptr;
    wnk::ui::Label *count_ = nullptr;
    wnk::ui::Label *message_ = nullptr;
    wnk::ui::CollectionView *grid_ = nullptr;
};

}  // namespace vplayer
