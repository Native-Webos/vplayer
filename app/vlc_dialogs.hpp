// SPDX-License-Identifier: GPL-3.0-or-later
//
// libVLC's dialogs (libvlc_dialog_set_callbacks) shown as the app's own: sign-in for a share or a
// server (user name, password, "remember", which libVLC keeps in its keystore), questions (an
// untrusted certificate: accept once, always, or not) and errors (a notice).
//
// libVLC asks from its own threads and waits for the answer; the dialogs are shown on the main
// thread through the dispatcher. Every dialog is answered exactly once: a button, Back (dismissed),
// or libVLC cancelling it (the request went away), which closes it.
#pragma once

#include "wnk/async/async.hpp"
#include "wnk/ui/navigation.hpp"

#include <functional>
#include <map>
#include <set>
#include <memory>
#include <string>

struct libvlc_instance_t;
struct libvlc_dialog_id;

namespace vplayer {

class VlcDialogs {
 public:
    using UiGetter = std::function<wnk::ui::Ui &()>;
    VlcDialogs(libvlc_instance_t *vlc, wnk::async::MainDispatcher &main, UiGetter ui);
    // Main thread: called when the viewer cancels a sign-in (Cancel or Back).
    std::function<void()> on_login_cancelled;
    ~VlcDialogs();
    VlcDialogs(const VlcDialogs &) = delete;
    VlcDialogs &operator=(const VlcDialogs &) = delete;

 private:
    // Main thread.
    void show_login(libvlc_dialog_id *id, const std::string &title, const std::string &text, const std::string &user,
                    bool ask_store);
    void show_question(libvlc_dialog_id *id, const std::string &title, const std::string &text, const std::string &cancel,
                       const std::string &action1, const std::string &action2);
    void cancel(libvlc_dialog_id *id);

    libvlc_instance_t *vlc_;
    wnk::async::MainDispatcher &main_;
    UiGetter ui_;
    std::shared_ptr<bool> alive_;                     // tasks posted to the main thread check it
    std::map<libvlc_dialog_id *, wnk::ui::Dialog *> open_;  // main thread
    std::set<libvlc_dialog_id *> progress_;                 // main thread: not shown, dismissed on cancel

    friend struct VlcDialogsCallbacks;
};

}  // namespace vplayer
