// SPDX-License-Identifier: GPL-3.0-or-later
#include "vlc_engine.hpp"

#include "wnk/log.hpp"

#include <cstdlib>

#if VPLAYER_HAVE_LIBVLC
#include <vlc/vlc.h>

#include "wnk_keystore.h"
#include "wnk/storage/secret.hpp"

#include <cstring>
#include <iterator>

#include <cstdio>
#endif

namespace vplayer {

namespace {
constexpr const char *kCat = "vlc";
}

std::string loggable(const std::string &text) {
    std::string out = text;
    // Each address in the text: "scheme://user:secret@host" keeps all but the secret.
    for (size_t scheme = out.find("://"); scheme != std::string::npos; scheme = out.find("://", scheme + 3)) {
        const size_t start = scheme + 3;
        const size_t end = out.find_first_of("/ '\"\n", start);  // the authority ends here
        const size_t at = out.find('@', start);
        if (at == std::string::npos || (end != std::string::npos && at > end)) continue;  // no user part
        const size_t colon = out.find(':', start);
        if (colon == std::string::npos || colon > at) continue;  // a user, no password
        out.replace(colon + 1, at - colon - 1, "***");
    }
    return out;
}

std::string file_uri(const std::string &path) {
    if (path.find("://") != std::string::npos) return path;  // already an address (smb://, http://...)
    static const char hex[] = "0123456789ABCDEF";
    std::string out = "file://";
    for (const char ch : path) {
        const auto c = static_cast<unsigned char>(ch);
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '/' || c == '-' ||
            c == '_' || c == '.' || c == '~') {
            out += ch;
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

#if VPLAYER_HAVE_LIBVLC

namespace {

// libVLC's log, into the app's event log. Debug lines are dropped: with ~300 modules probing they
// are thousands per file opened.
void on_vlc_log(void *, int level, const libvlc_log_t *ctx, const char *fmt, va_list args) {
    if (level < LIBVLC_NOTICE) return;
    char msg[1024];
    va_list copy;
    va_copy(copy, args);
    std::vsnprintf(msg, sizeof msg, fmt, copy);
    va_end(copy);
    const char *module = nullptr, *file = nullptr;
    unsigned line = 0;
    libvlc_log_get_context(ctx, &module, &file, &line);
    if (level >= LIBVLC_ERROR) {
        WNK_LOGE(kCat, "[%s] %s", module ? module : "?", msg);
    } else if (level >= LIBVLC_WARNING) {
        WNK_LOGW(kCat, "[%s] %s", module ? module : "?", msg);
    } else {
        WNK_LOGI(kCat, "[%s] %s", module ? module : "?", msg);
    }
}

}  // namespace

// VLC's saved sign-ins (modules/wnk_keystore): one secret holding the module's text.
struct VlcEngine::Signins {
    explicit Signins(wnk::storage::KeyValueStore &kv)
        : secrets(kv, wnk::storage::luna_keymanager3(), "com.sostk.vplayer.secrets.v1") {
        io.abi = WNK_KEYSTORE_ABI;
        io.opaque = this;
        io.load = [](void *opaque) -> char * {
            auto *self = static_cast<Signins *>(opaque);
            std::string text;
            const auto st = self->secrets.get(kSecret, &text);
            if (st != wnk::storage::SecretStatus::Ok) {
                if (st != wnk::storage::SecretStatus::NotFound)
                    WNK_LOGW(kCat, "saved sign-ins unreadable: %s", wnk::storage::secret_status_name(st));
                return nullptr;
            }
            return strdup(text.c_str());
        };
        io.save = [](void *opaque, const char *text) -> int {
            auto *self = static_cast<Signins *>(opaque);
            const auto st = text[0] == 0 ? self->secrets.remove(kSecret) : self->secrets.put(kSecret, text);
            const bool ok = st == wnk::storage::SecretStatus::Ok || st == wnk::storage::SecretStatus::NotFound;
            WNK_LOGI(kCat, "sign-ins saved (%s): %s", wnk::storage::protection_name(self->secrets.protection()),
                     ok ? "ok" : wnk::storage::secret_status_name(st));
            return ok ? 0 : -1;
        };
    }
    static constexpr const char *kSecret = "vlc.signins";
    wnk::storage::SecretStore secrets;
    wnk_keystore_io io{};
};

VlcEngine::VlcEngine(libvlc_instance_t *vlc, std::unique_ptr<Signins> signins, bool encrypted)
    : signins_(std::move(signins)), vlc_(vlc), encrypted_(encrypted) {}

std::unique_ptr<VlcEngine> VlcEngine::create(const std::string &app_dir, VlcInfo &info,
                                             wnk::storage::KeyValueStore *store) {
    // libvlccore finds lib/vlc/plugins next to itself (src/linux/dirs.c reads /proc/self/maps;
    // verified on an OLED65C1, webOS 6.5.3, 2026-11-24). VLC_PLUGIN_PATH must NOT be set as well:
    // VLC scans it IN ADDITION to that directory, so every module would load twice. The data dir
    // (Lua scripts) is derived by swapping /lib/ for /share/; it is stated explicitly.
    setenv("VLC_DATA_PATH", (app_dir + "/share/vlc").c_str(), 1);

    // Subtitles: without fontconfig, VLC's freetype renderer wants a font FILE, and its built-in
    // default is a desktop path the TV does not have ("Error loading default face"). Use the app's own.
    const std::string font = "--freetype-font=" + app_dir + "/fonts/Inter-SemiBold.ttf";
    const std::string mono = "--freetype-monofont=" + app_dir + "/fonts/Inter-Regular.ttf";
    // Saved sign-ins: VLC's keystore is our module, storing through the kit's secret store.
    std::unique_ptr<Signins> signins;
    bool encrypted = false;
    std::string keystore_io;
    if (store != nullptr) {
        signins = std::make_unique<Signins>(*store);
        encrypted = signins->secrets.protection() == wnk::storage::Protection::Keymanager3;
        char buf[64];
        std::snprintf(buf, sizeof buf, "--wnk-keystore=%p", static_cast<void *>(&signins->io));
        keystore_io = buf;
        WNK_LOGI(kCat, "sign-ins kept across launches, %s", encrypted ? "encrypted (keymanager3)" : "unencrypted");
    }
    std::vector<const char *> extra;
    if (signins) {
        extra.push_back("--keystore=wnk_keystore");
        extra.push_back(keystore_io.c_str());
    }
    const char *const base_args[] = {
        "--no-video-title-show",  // the app draws its own title
        "--stats",  // the player's Info tab shows frames shown and dropped, and the input bitrate
        "--no-lua",               // no Lua interfaces/extensions yet; playlist parsers still load
        font.c_str(),
        mono.c_str(),
        "--freetype-outline-thickness=4",
        "--freetype-rel-fontsize=12",
        // Software decoding on a 3-core TV CPU: allow FFmpeg's non-bit-exact speedups and skip the
        // H.264/HEVC deblocking filter on non-reference frames (invisible in motion, ~20-30% less work).
        "--avcodec-fast",
        "--avcodec-skiploopfilter=1",  // VLC's "large" (smaller numbers are bigger): TV viewing distance
        // HTTPS: GnuTLS's built-in system store path is not webOS's, so every certificate was "not
        // trusted"; the TV keeps its CAs as PEM files in /etc/ssl/certs.
        "--no-gnutls-system-trust",
        "--gnutls-dir-trust=/etc/ssl/certs",
    };
    std::vector<const char *> args(std::begin(base_args), std::end(base_args));
    args.insert(args.end(), extra.begin(), extra.end());
    const auto start = std::chrono::steady_clock::now();
    libvlc_instance_t *vlc = libvlc_new(static_cast<int>(args.size()), args.data());
    info.init_time = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
    info.version = libvlc_get_version();
    info.compiler = libvlc_get_compiler();
    if (!vlc) {
        const char *err = libvlc_errmsg();
        info.error = err ? err : "libvlc_new failed";
        WNK_LOGE(kCat, "libvlc_new failed after %lld ms: %s", (long long)info.init_time.count(), info.error.c_str());
        return nullptr;
    }
    libvlc_log_set(vlc, on_vlc_log, nullptr);
    libvlc_set_user_agent(vlc, "VPlayer", "VPlayer/0.1 LibVLC");
    libvlc_set_app_id(vlc, "com.sostk.vplayer", "0.1.0", "");

    if (libvlc_audio_output_t *list = libvlc_audio_output_list_get(vlc)) {
        for (auto *o = list; o; o = o->p_next) info.audio_outputs.emplace_back(o->psz_name);
        libvlc_audio_output_list_release(list);
    }
    info.available = true;
    WNK_LOGI(kCat, "libVLC %s (%s) ready in %lld ms, %zu audio outputs", info.version.c_str(),
             info.compiler.c_str(), (long long)info.init_time.count(), info.audio_outputs.size());
    return std::unique_ptr<VlcEngine>(new VlcEngine(vlc, std::move(signins), encrypted));
}

VlcEngine::~VlcEngine() {
    if (vlc_) libvlc_release(vlc_);
}

std::shared_ptr<libvlc_instance_t> hold_vlc(libvlc_instance_t *vlc) {
    if (vlc == nullptr) return nullptr;
    libvlc_retain(vlc);
    return std::shared_ptr<libvlc_instance_t>(vlc, [](libvlc_instance_t *v) { libvlc_release(v); });
}

#else  // !VPLAYER_HAVE_LIBVLC

std::shared_ptr<libvlc_instance_t> hold_vlc(libvlc_instance_t *) { return nullptr; }

struct VlcEngine::Signins {};

std::unique_ptr<VlcEngine> VlcEngine::create(const std::string &, VlcInfo &info, wnk::storage::KeyValueStore *) {
    info.error = "this build has no libVLC (host build)";
    WNK_LOGI(kCat, "%s", info.error.c_str());
    return nullptr;
}

VlcEngine::~VlcEngine() = default;

#endif

}  // namespace vplayer
