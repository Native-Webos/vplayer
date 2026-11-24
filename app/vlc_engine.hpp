// SPDX-License-Identifier: GPL-3.0-or-later
//
// VlcEngine: the one libvlc_instance_t of the app.
//
// libVLC is bundled in the package (lib/libvlc.so.5, lib/libvlccore.so.9, lib/vlc/plugins,
// share/vlc/lua; see libvlc/inside/vlc.sh). Builds without it (the desktop host build, until a host
// libVLC is wired up) compile this file to a stub whose create() reports "not available", so the
// UI and its tests run everywhere.
//
// Threading: create() blocks (libvlc_new scans ~300 plugins without a cache), so call it on a
// worker. Everything else may be called from any thread; libVLC's own API is thread-safe.
#pragma once

#include <chrono>
#include <memory>
#include <string>
#include <vector>

struct libvlc_instance_t;

namespace vplayer {

struct VlcInfo {
    bool available = false;
    std::string error;             // why not, when !available
    std::string version;           // libvlc_get_version()
    std::string compiler;          // libvlc_get_compiler()
    std::chrono::milliseconds init_time{0};
    std::vector<std::string> audio_outputs;  // module names libVLC could load (pulse, alsa, ...)
};

// An absolute local path as a file:// URI for libVLC (percent-encoded: spaces, '#', '%', non-ASCII).
std::string file_uri(const std::string &path);

class VlcEngine {
 public:
    // app_dir: the directory holding the executable (LaunchInfo::app_dir).
    static std::unique_ptr<VlcEngine> create(const std::string &app_dir, VlcInfo &info);
    ~VlcEngine();
    VlcEngine(const VlcEngine &) = delete;
    VlcEngine &operator=(const VlcEngine &) = delete;

    libvlc_instance_t *instance() const { return vlc_; }

 private:
    explicit VlcEngine(libvlc_instance_t *vlc) : vlc_(vlc) {}
    libvlc_instance_t *vlc_ = nullptr;
};

}  // namespace vplayer
