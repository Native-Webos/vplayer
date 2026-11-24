// SPDX-License-Identifier: GPL-3.0-or-later
#include "vlc_dialogs.hpp"

#include "vlc_engine.hpp"

#include "wnk/log.hpp"
#include "wnk/ui/forms.hpp"
#include "wnk/ui/widgets.hpp"

#include <set>

#if VPLAYER_HAVE_LIBVLC
#include <vlc/vlc.h>
#endif

namespace vplayer {

namespace {
constexpr const char *kCat = "dialogs";
std::string str(const char *s) { return s ? s : ""; }
}  // namespace

#if VPLAYER_HAVE_LIBVLC

// libVLC's callbacks (its threads): everything moves to the main thread.
struct VlcDialogsCallbacks {
    static VlcDialogs *self(void *p) { return static_cast<VlcDialogs *>(p); }

    static void error(void *p, const char *title, const char *text) {
        auto *s = self(p);
        std::string msg = str(title);
        if (text && *text) msg += (msg.empty() ? "" : ": ") + str(text);
        msg = loggable(msg);  // VLC's messages quote the address, password and all
        WNK_LOGW(kCat, "libVLC error: %s", msg.c_str());
        s->main_.post([s, alive = std::weak_ptr<bool>(s->alive_), msg] {
            if (alive.lock()) s->ui_().toast(msg, 5000);
        });
    }
    static void login(void *p, libvlc_dialog_id *id, const char *title, const char *text, const char *user,
                      bool ask_store) {
        auto *s = self(p);
        s->main_.post([s, alive = std::weak_ptr<bool>(s->alive_), id, t = str(title), x = str(text), u = str(user),
                       ask_store] {
            if (alive.lock()) {
                s->show_login(id, t, x, u, ask_store);
            } else {
                libvlc_dialog_dismiss(id);
            }
        });
    }
    static void question(void *p, libvlc_dialog_id *id, const char *title, const char *text,
                         libvlc_dialog_question_type, const char *cancel, const char *action1, const char *action2) {
        auto *s = self(p);
        s->main_.post([s, alive = std::weak_ptr<bool>(s->alive_), id, t = str(title), x = str(text), c = str(cancel),
                       a1 = str(action1), a2 = str(action2)] {
            if (alive.lock()) {
                s->show_question(id, t, x, c, a1, a2);
            } else {
                libvlc_dialog_dismiss(id);
            }
        });
    }
    // Progress is not shown (the pages have their own spinners); libVLC cancels it when done, and the
    // id must then be dismissed to be released.
    static void progress(void *p, libvlc_dialog_id *id, const char *, const char *, bool, float, const char *) {
        auto *s = self(p);
        s->main_.post([s, alive = std::weak_ptr<bool>(s->alive_), id] {
            if (alive.lock()) s->progress_.insert(id);
        });
    }
    static void update_progress(void *, libvlc_dialog_id *, float, const char *) {}
    static void cancel(void *p, libvlc_dialog_id *id) {
        auto *s = self(p);
        s->main_.post([s, alive = std::weak_ptr<bool>(s->alive_), id] {
            if (alive.lock()) s->cancel(id);
        });
    }
};

namespace {
const libvlc_dialog_cbs kCallbacks = {
    &VlcDialogsCallbacks::error,    &VlcDialogsCallbacks::login,  &VlcDialogsCallbacks::question,
    &VlcDialogsCallbacks::progress, &VlcDialogsCallbacks::cancel, &VlcDialogsCallbacks::update_progress,
};
}  // namespace

VlcDialogs::VlcDialogs(libvlc_instance_t *vlc, wnk::async::MainDispatcher &main, UiGetter ui)
    : vlc_(vlc), main_(main), ui_(std::move(ui)), alive_(std::make_shared<bool>(true)) {
    if (vlc_ != nullptr) libvlc_dialog_set_callbacks(vlc_, &kCallbacks, this);
}

VlcDialogs::~VlcDialogs() {
    if (vlc_ != nullptr) libvlc_dialog_set_callbacks(vlc_, nullptr, nullptr);
    alive_.reset();
    for (auto &d : open_) libvlc_dialog_dismiss(d.first);
}

void VlcDialogs::show_login(libvlc_dialog_id *id, const std::string &title, const std::string &text,
                            const std::string &user, bool ask_store) {
    using namespace wnk::ui;
    auto d = std::make_unique<Dialog>(title.empty() ? "Sign in" : title, text);
    auto &name = d->body().add<TextField>("User name");
    name.set_text(user);
    auto &pass = d->body().add<TextField>("Password");
    pass.set_password(true);
    Toggle *remember = ask_store ? &d->body().add<Toggle>("Remember on this TV", true) : nullptr;
    auto answered = std::make_shared<bool>(false);
    d->add_button("Sign in", [this, id, answered, &name, &pass, remember](Ui &) {
        *answered = true;
        libvlc_dialog_post_login(id, name.text().c_str(), pass.text().c_str(), remember != nullptr && remember->on());
        WNK_LOGI(kCat, "sign-in sent for %s", name.text().c_str());
    });
    d->add_button("Cancel", [this, answered, id](Ui &) {
        *answered = true;
        libvlc_dialog_dismiss(id);
        if (on_login_cancelled) on_login_cancelled();
    });
    d->on_dismiss = [this, id, answered](Ui &) {
        if (!*answered) {  // Back
            libvlc_dialog_dismiss(id);
            if (on_login_cancelled) on_login_cancelled();
        }
        open_.erase(id);
    };
    open_[id] = &ui_().show_dialog(std::move(d));
}

void VlcDialogs::show_question(libvlc_dialog_id *id, const std::string &title, const std::string &text,
                               const std::string &cancel_text, const std::string &action1, const std::string &action2) {
    using namespace wnk::ui;
    auto d = std::make_unique<Dialog>(title, text);
    auto answered = std::make_shared<bool>(false);
    if (!action1.empty())
        d->add_button(action1, [id, answered](Ui &) {
            *answered = true;
            libvlc_dialog_post_action(id, 1);
        });
    if (!action2.empty())
        d->add_button(action2, [id, answered](Ui &) {
            *answered = true;
            libvlc_dialog_post_action(id, 2);
        });
    d->add_button(cancel_text.empty() ? "Cancel" : cancel_text, [id, answered](Ui &) {
        *answered = true;
        libvlc_dialog_dismiss(id);
    });
    d->on_dismiss = [this, id, answered](Ui &) {
        if (!*answered) libvlc_dialog_dismiss(id);
        open_.erase(id);
    };
    open_[id] = &ui_().show_dialog(std::move(d));
}

void VlcDialogs::cancel(libvlc_dialog_id *id) {
    if (progress_.erase(id) > 0) {
        libvlc_dialog_dismiss(id);
        return;
    }
    const auto it = open_.find(id);
    if (it != open_.end()) it->second->dismiss(ui_());  // on_dismiss dismisses the id
}

#else  // no libVLC

VlcDialogs::VlcDialogs(libvlc_instance_t *vlc, wnk::async::MainDispatcher &main, UiGetter ui)
    : vlc_(vlc), main_(main), ui_(std::move(ui)) {}
VlcDialogs::~VlcDialogs() = default;
void VlcDialogs::show_login(libvlc_dialog_id *, const std::string &, const std::string &, const std::string &, bool) {}
void VlcDialogs::show_question(libvlc_dialog_id *, const std::string &, const std::string &, const std::string &,
                               const std::string &, const std::string &) {}
void VlcDialogs::cancel(libvlc_dialog_id *) {}

#endif

}  // namespace vplayer
