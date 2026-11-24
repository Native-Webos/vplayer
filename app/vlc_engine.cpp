// SPDX-License-Identifier: GPL-3.0-or-later
#include "vlc_engine.hpp"

#include "wnk/log.hpp"

#include <cstdlib>

#if VPLAYER_HAVE_LIBVLC
#include <vlc/vlc.h>

#include <cstdio>
#endif

namespace vplayer {

namespace {
constexpr const char *kCat = "vlc";
}

std::string file_uri(const std::string &path) {
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

std::unique_ptr<VlcEngine> VlcEngine::create(const std::string &app_dir, VlcInfo &info) {
    // libvlccore finds lib/vlc/plugins next to itself (src/linux/dirs.c reads /proc/self/maps;
    // verified on an OLED65C1, webOS 6.5.3, 2026-11-24). VLC_PLUGIN_PATH must NOT be set as well:
    // VLC scans it IN ADDITION to that directory, so every module would load twice. The data dir
    // (Lua scripts) is derived by swapping /lib/ for /share/; it is stated explicitly.
    setenv("VLC_DATA_PATH", (app_dir + "/share/vlc").c_str(), 1);

    // Subtitles: without fontconfig, VLC's freetype renderer wants a font FILE, and its built-in
    // default is a desktop path the TV does not have ("Error loading default face"). Use the app's own.
    const std::string font = "--freetype-font=" + app_dir + "/fonts/Inter-SemiBold.ttf";
    const std::string mono = "--freetype-monofont=" + app_dir + "/fonts/Inter-Regular.ttf";
    const char *const args[] = {
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
    };
    const auto start = std::chrono::steady_clock::now();
    libvlc_instance_t *vlc = libvlc_new(sizeof args / sizeof *args, args);
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
    return std::unique_ptr<VlcEngine>(new VlcEngine(vlc));
}

VlcEngine::~VlcEngine() {
    if (vlc_) libvlc_release(vlc_);
}

#else  // !VPLAYER_HAVE_LIBVLC

std::unique_ptr<VlcEngine> VlcEngine::create(const std::string &, VlcInfo &info) {
    info.error = "this build has no libVLC (host build)";
    WNK_LOGI(kCat, "%s", info.error.c_str());
    return nullptr;
}

VlcEngine::~VlcEngine() = default;

#endif

}  // namespace vplayer
