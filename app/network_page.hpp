// SPDX-License-Identifier: GPL-3.0-or-later
//
// Network tab: open a stream by its address, add a server by address, servers found on the local
// network (DLNA, and SMB/NFS/FTP servers that announce themselves), then their folders and files.
// Back goes up a folder, then back to the servers. A file plays with the folder's other playable
// entries as the queue.
//
// The red key on a saved server or a recent address forgets it.
#pragma once

#include "network.hpp"
#include "pages.hpp"

#include <memory>
#include <string>
#include <vector>

namespace vplayer {

class NetworkPage : public Page {
 public:
    NetworkPage(Services &services, libvlc_instance_t *vlc);
    void on_shown(wnk::ui::Ui &ui) override;
    void update(wnk::ui::Ui &ui, float dt) override;
    bool on_action(wnk::ui::Ui &ui, const wnk::input::ActionEvent &e) override;
    // libVLC became available after the page was made (it starts on a worker).
    void set_vlc(libvlc_instance_t *vlc);
    // "Open a network stream": an address, played at once and remembered.
    void open_url_dialog(wnk::ui::Ui &ui) { ask_url(ui); }

 private:
    enum class Item : uint8_t { OpenUrl, AddServer, Manage, Saved, Found, Recent };
    struct RootItem {
        Item what;
        NetEntry entry;  // Saved/Found: the server; Recent: name = mrl = the address
    };

    void show_root(wnk::ui::Ui &ui, bool keep_focus);
    void open(wnk::ui::Ui &ui, const NetEntry &server, const std::string &mrl, const std::string &focus_mrl = {});
    void show_entries(wnk::ui::Ui &ui, std::vector<NetEntry> entries, const std::string &error, const std::string &focus_mrl);
    void set_crumbs();
    void activate(wnk::ui::Ui &ui, size_t i);
    void ask_url(wnk::ui::Ui &ui);
    void ask_server(wnk::ui::Ui &ui);
    void forget(wnk::ui::Ui &ui, size_t i);
    // Manage saved: the saved servers and addresses, each removable (after a confirmation).
    void manage_saved(wnk::ui::Ui &ui);
    void confirm_remove(wnk::ui::Ui &ui, bool server, const NetEntry &entry);
    void remove_place(wnk::ui::Ui &ui, bool server, const std::string &mrl, const std::string &name);
    void play_url(wnk::ui::Ui &ui, const std::string &url);
    void save_places();

    Services &s_;
    std::unique_ptr<NetworkBrowser> browser_;
    wnk::async::Lifetime lifetime_;
    wnk::async::CancellationSource listing_;
    SavedPlaces places_;
    bool places_loaded_ = false;
    std::vector<NetEntry> found_;
    std::vector<RootItem> root_;
    std::vector<NetEntry> entries_;
    NetEntry server_;        // the server being browsed
    std::string folder_;     // its folder shown; empty: the root list
    float poll_ = 0;
    bool want_focus_ = false;
    wnk::ui::Row *crumbs_ = nullptr;
    wnk::ui::Label *count_ = nullptr;
    wnk::ui::Label *message_ = nullptr;
    wnk::ui::Label *hint_ = nullptr;
    wnk::ui::CollectionView *grid_ = nullptr;
};

}  // namespace vplayer
