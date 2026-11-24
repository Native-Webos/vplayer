// SPDX-License-Identifier: GPL-3.0-or-later
#include "thumbnails.hpp"

#include "wnk/log.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sys/stat.h>
#include <vector>

#if VPLAYER_HAVE_LIBVLC
#include <vlc/vlc.h>
#endif

namespace vplayer {

namespace {

constexpr const char *kCat = "thumbs";
constexpr int kWidth = 320;

void put16(std::string &s, uint32_t v) {
    s += static_cast<char>(v & 0xFF);
    s += static_cast<char>(v >> 8 & 0xFF);
}
void put32(std::string &s, uint32_t v) {
    put16(s, v & 0xFFFF);
    put16(s, v >> 16);
}

void make_dirs(const std::string &dir) {
    for (size_t p = 1; p != std::string::npos; p = dir.find('/', p + 1)) ::mkdir(dir.substr(0, p).c_str(), 0755);
    ::mkdir(dir.c_str(), 0755);
}

bool exists(const std::string &path) {
    struct stat st {};
    return ::stat(path.c_str(), &st) == 0 && st.st_size > 0;
}

}  // namespace

std::string thumbnail_name(const std::string &path, uint64_t size, int64_t modified_s) {
    uint64_t h = 1469598103934665603ull;  // FNV-1a over path, size and date
    auto mix = [&h](const void *p, size_t n) {
        const auto *b = static_cast<const uint8_t *>(p);
        for (size_t i = 0; i < n; ++i) h = (h ^ b[i]) * 1099511628211ull;
    };
    mix(path.data(), path.size());
    mix(&size, sizeof size);
    mix(&modified_s, sizeof modified_s);
    char buf[32];
    std::snprintf(buf, sizeof buf, "%016llx.bmp", static_cast<unsigned long long>(h));
    return buf;
}

std::string bmp_from_bgra(const uint8_t *px, int w, int h) {
    std::string s;
    const uint32_t image = static_cast<uint32_t>(w) * static_cast<uint32_t>(h) * 4;
    s += "BM";
    put32(s, 54 + image);
    put32(s, 0);
    put32(s, 54);
    put32(s, 40);  // BITMAPINFOHEADER
    put32(s, static_cast<uint32_t>(w));
    put32(s, static_cast<uint32_t>(h));  // positive: bottom-up rows
    put16(s, 1);
    put16(s, 32);
    put32(s, 0);  // BI_RGB
    put32(s, image);
    put32(s, 2835);
    put32(s, 2835);
    put32(s, 0);
    put32(s, 0);
    for (int y = h - 1; y >= 0; --y) {
        const uint8_t *row = px + static_cast<size_t>(y) * static_cast<size_t>(w) * 4;
        for (int x = 0; x < w; ++x) {
            s += static_cast<char>(row[x * 4]);
            s += static_cast<char>(row[x * 4 + 1]);
            s += static_cast<char>(row[x * 4 + 2]);
            s += static_cast<char>(0xFF);  // opaque
        }
    }
    return s;
}

Thumbnails::Thumbnails(libvlc_instance_t *vlc, std::string cache_dir, wnk::async::MainDispatcher &main)
    : vlc_(vlc), dir_(std::move(cache_dir)), main_(main) {
    if (vlc_ == nullptr || dir_.empty()) return;
    make_dirs(dir_);
    thread_ = std::thread([this] { run(); });
}

Thumbnails::~Thumbnails() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        stop_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
    alive_.reset();
}

std::string Thumbnails::get(const wnk::platform::FileEntry &e) {
    if (vlc_ == nullptr || e.kind != wnk::platform::FileKind::Video || e.path.find("://") != std::string::npos) return {};
    if (auto it = ready_.find(e.path); it != ready_.end()) return it->second;
    const std::string file = dir_ + "/" + thumbnail_name(e.path, e.size, e.modified_s);
    if (exists(file)) {
        ready_[e.path] = "file://" + file;
        return ready_[e.path];
    }
    std::lock_guard<std::mutex> lock(mu_);
    if (!failed_.count(e.path) && queued_.insert(e.path).second) {
        queue_.emplace_back(e.path, file);
        cv_.notify_all();
    }
    return {};
}

void Thumbnails::set_paused(bool paused) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        paused_ = paused;
    }
    cv_.notify_all();
}

void Thumbnails::run() {
    for (;;) {
        std::pair<std::string, std::string> job;
        {
            std::unique_lock<std::mutex> lock(mu_);
            cv_.wait(lock, [this] { return stop_ || (!paused_ && !queue_.empty()); });
            if (stop_) return;
            job = std::move(queue_.front());
            queue_.pop_front();
        }
        const bool ok = make(job.first, job.second);
        std::lock_guard<std::mutex> lock(mu_);
        queued_.erase(job.first);
        if (!ok) {
            failed_.insert(job.first);
            continue;
        }
        main_.post([this, alive = std::weak_ptr<bool>(alive_), path = job.first, url = "file://" + job.second] {
            if (!alive.lock()) return;
            ready_[path] = url;
            ++generation_;
        });
    }
}

#if VPLAYER_HAVE_LIBVLC

namespace {

struct Grab {
    std::mutex mu;
    std::condition_variable cv;
    std::vector<uint8_t> pixels;
    int width = 0, height = 0;
    int shown = 0;
};

void *grab_lock(void *opaque, void **planes) {
    auto *g = static_cast<Grab *>(opaque);
    planes[0] = g->pixels.data();
    return nullptr;
}
void grab_unlock(void *, void *, void *const *) {}
void grab_display(void *opaque, void *) {
    auto *g = static_cast<Grab *>(opaque);
    std::lock_guard<std::mutex> lock(g->mu);
    ++g->shown;
    g->cv.notify_all();
}

}  // namespace

bool Thumbnails::make(const std::string &path, const std::string &out) {
    const auto t0 = std::chrono::steady_clock::now();
    libvlc_media_t *m = libvlc_media_new_path(vlc_, path.c_str());
    if (m == nullptr) return false;
    // The picture's size and the duration, for the position and the scaled height.
    libvlc_media_parse_with_options(m, libvlc_media_parse_local, 3000);
    for (int i = 0; i < 350 && libvlc_media_get_parsed_status(m) == 0; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    int vw = 0, vh = 0;
    libvlc_media_track_t **tracks = nullptr;
    const unsigned n = libvlc_media_tracks_get(m, &tracks);
    for (unsigned i = 0; i < n; ++i)
        if (tracks[i]->i_type == libvlc_track_video && tracks[i]->video && vw == 0) {
            vw = static_cast<int>(tracks[i]->video->i_width);
            vh = static_cast<int>(tracks[i]->video->i_height);
        }
    if (n > 0) libvlc_media_tracks_release(tracks, n);
    if (vw <= 0 || vh <= 0) {
        libvlc_media_release(m);
        return false;
    }
    const libvlc_time_t duration = libvlc_media_get_duration(m);
    for (const char *o : {":no-audio", ":no-spu", ":no-sub-autodetect-file", ":input-fast-seek", ":avcodec-hw=none",
                          ":no-video-title-show"})
        libvlc_media_add_option(m, o);
    if (duration > 0) {
        const double start = std::min(duration * 0.2 / 1000.0, 600.0);
        libvlc_media_add_option(m, (":start-time=" + std::to_string(start)).c_str());
    }

    Grab g;
    g.width = kWidth;
    g.height = std::max(2, (kWidth * vh / vw) & ~1);
    g.pixels.assign(static_cast<size_t>(g.width) * static_cast<size_t>(g.height) * 4, 0);
    libvlc_media_player_t *mp = libvlc_media_player_new_from_media(m);
    libvlc_media_release(m);
    if (mp == nullptr) return false;
    libvlc_video_set_callbacks(mp, &grab_lock, &grab_unlock, &grab_display, &g);
    libvlc_video_set_format(mp, "RV32", static_cast<unsigned>(g.width), static_cast<unsigned>(g.height),
                            static_cast<unsigned>(g.width * 4));
    bool got = false;
    if (libvlc_media_player_play(mp) == 0) {
        std::unique_lock<std::mutex> lock(g.mu);
        got = g.cv.wait_for(lock, std::chrono::seconds(8), [&] { return g.shown >= 1; });
    }
    libvlc_media_player_stop(mp);
    libvlc_media_player_release(mp);
    if (!got) {
        WNK_LOGW(kCat, "no picture from %s", path.c_str());
        return false;
    }
    const std::string bmp = bmp_from_bgra(g.pixels.data(), g.width, g.height);
    const std::string tmp = out + ".part";
    {
        std::ofstream f(tmp, std::ios::binary);
        f.write(bmp.data(), static_cast<std::streamsize>(bmp.size()));
        if (!f) return false;
    }
    if (std::rename(tmp.c_str(), out.c_str()) != 0) return false;
    WNK_LOGI(kCat, "%s in %lld ms", path.c_str(),
             static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count()));
    return true;
}

#else

bool Thumbnails::make(const std::string &, const std::string &) { return false; }

#endif

}  // namespace vplayer
