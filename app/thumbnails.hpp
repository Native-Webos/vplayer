// SPDX-License-Identifier: GPL-3.0-or-later
//
// Video thumbnails, made with libVLC (version 3 has no thumbnailer of its own): one file at a time
// on a thread of their own, a hidden player with audio and subtitles off decodes in software from a
// fifth of the way in (at most 10 minutes) and hands the first picture over through libVLC's video
// callbacks, scaled by VLC to 320 wide. The picture is written as a BMP (which the kit's image loader
// reads) in a cache folder, named after the file's path, size and date, so a changed file gets a
// new one. A file that fails is not tried again this run.
//
// While a player screen is open thumbnailing pauses (set_paused), so it never competes with playback.
// Builds without libVLC (the host) make none.
#pragma once

#include "wnk/async/async.hpp"
#include "wnk/platform/files.hpp"

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>

struct libvlc_instance_t;

namespace vplayer {

// The cache file name for a file (pure: tested on the host).
std::string thumbnail_name(const std::string &path, uint64_t size, int64_t modified_s);
// A 32-bit BMP of `width`×`height` BGRA rows (top row first), as written to the cache.
std::string bmp_from_bgra(const uint8_t *pixels, int width, int height);

class Thumbnails {
 public:
    // `cache_dir` is created if missing. `vlc` may be null (no thumbnails).
    Thumbnails(libvlc_instance_t *vlc, std::string cache_dir, wnk::async::MainDispatcher &main);
    ~Thumbnails();
    Thumbnails(const Thumbnails &) = delete;
    Thumbnails &operator=(const Thumbnails &) = delete;

    // Main thread: the file:// URL of `e`'s thumbnail if there is one; else queues it (videos only)
    // and returns empty. generation() bumps when one becomes ready.
    std::string get(const wnk::platform::FileEntry &e);
    uint64_t generation() const { return generation_; }
    void set_paused(bool paused);

 private:
    void run();
    bool make(const std::string &path, const std::string &out);

    libvlc_instance_t *vlc_;
    std::string dir_;
    wnk::async::MainDispatcher &main_;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
    uint64_t generation_ = 0;                  // main thread
    std::map<std::string, std::string> ready_;  // path → URL (main thread)

    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<std::pair<std::string, std::string>> queue_;  // path, cache file
    std::set<std::string> queued_, failed_;
    bool paused_ = false, stop_ = false;
    std::thread thread_;
};

}  // namespace vplayer
