// SPDX-License-Identifier: GPL-3.0-or-later
//
// libVLC playback, software path: VLC demuxes and decodes; its video output is libvlc's "vmem"
// callbacks in I420, copied into a FrameSink; audio is VLC's own (PulseAudio on the TV). VLC also
// blends subtitles and runs its video filters (deinterlace, adjust) before the callback.
#include "player/playback.hpp"
#include "player/vlc_vars.hpp"
#include "vlc_engine.hpp"

#include "wnk/log.hpp"

#include <algorithm>
#include <chrono>
#include <atomic>
#include <cstdio>
#include <cstring>

#if VPLAYER_HAVE_LIBVLC
#include <vlc/vlc.h>
#endif

namespace vplayer {

#if VPLAYER_HAVE_LIBVLC

namespace {

constexpr const char *kCat = "player";

std::string fourcc_text(uint32_t f) {
    char s[5] = {static_cast<char>(f & 0xFF), static_cast<char>((f >> 8) & 0xFF), static_cast<char>((f >> 16) & 0xFF),
                 static_cast<char>((f >> 24) & 0xFF), 0};
    std::string out(s);
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

std::string codec_name(uint32_t fourcc) {
    switch (fourcc) {
        case 0x34363268: return "H.264";      // 'h264'
        case 0x63766568: return "HEVC";       // 'hevc'
        case 0x30395056: return "VP9";        // 'VP90'
        case 0x30385056: return "VP8";        // 'VP80'
        case 0x31307661: return "AV1";        // 'av01'
        case 0x7667706d: return "MPEG-2";     // 'mpgv'
        case 0x7634706d: return "MPEG-4";     // 'mp4v'
        case 0x31435657: return "VC-1";       // 'WVC1'
        case 0x6f656874: return "Theora";     // 'theo'
        case 0x6134706d: return "AAC";        // 'mp4a'
        case 0x20323561: return "AC-3";       // 'a52 '
        case 0x33636165: return "E-AC-3";     // 'eac3'
        case 0x20737464: return "DTS";        // 'dts '
        case 0x6167706d: return "MPEG audio"; // 'mpga'
        case 0x7375706f: return "Opus";       // 'opus'
        case 0x62726f76: return "Vorbis";     // 'vorb'
        case 0x63616c66: return "FLAC";       // 'flac'
        case 0x20706c6d: return "MLP";        // 'mlp '
        case 0x64687274: return "TrueHD";     // 'trhd'
        case 0x20736d77: case 0x31616d77: case 0x32616d77: return "WMA";  // 'wms ', 'wma1', 'wma2'
        case 0x76616c61: return "ALAC";       // 'alac'
        case 0x6d637061: return "PCM";        // 'apcm'
        case 0x74627573: return "SubRip";     // 'subt'
        case 0x20617373: return "ASS";        // 'ssa '
        default: return fourcc_text(fourcc);
    }
}

std::vector<TrackOption> to_options(libvlc_track_description_t *list) {
    std::vector<TrackOption> out;
    for (auto *d = list; d; d = d->p_next) out.push_back({d->i_id, d->psz_name ? d->psz_name : "Track"});
    if (list) libvlc_track_description_list_release(list);
    return out;
}

class VlcPlayback final : public Playback {
 public:
    VlcPlayback(libvlc_instance_t *vlc, const std::string &mrl, const std::vector<std::string> &options,
                const AudioEffects &effects)
        : sink_(std::make_shared<FrameSink>()), opened_(effects) {
        const bool url = mrl.find("://") != std::string::npos;
        libvlc_media_t *m = url ? libvlc_media_new_location(vlc, mrl.c_str()) : libvlc_media_new_path(vlc, mrl.c_str());
        if (m == nullptr) {
            error_ = "cannot open " + mrl;
            return;
        }
        for (const auto &o : options) {
            libvlc_media_add_option(m, o.c_str());
            WNK_LOGI(kCat, "vlc: option %s", o.c_str());
        }
        mp_ = libvlc_media_player_new_from_media(m);
        libvlc_media_release(m);
        if (mp_ == nullptr) {
            error_ = "cannot create a player";
            return;
        }
        libvlc_video_set_callbacks(mp_, &VlcPlayback::lock_cb, &VlcPlayback::unlock_cb, &VlcPlayback::display_cb, this);
        libvlc_video_set_format_callbacks(mp_, &VlcPlayback::setup_cb, &VlcPlayback::cleanup_cb);
        // The audio effects before play: the audio output takes the player's filters when it is made.
        if (effects.volume != 100) libvlc_audio_set_volume(mp_, std::clamp(effects.volume, 0, 200));
        if (effects.equalizer >= 0) apply_equalizer(effects);
        if (effects.normalize || effects.widen) {
            std::string filters = effects.equalizer >= 0 ? "equalizer" : "";
            if (effects.normalize) filters += std::string(filters.empty() ? "" : ":") + "normvol";
            if (effects.widen) filters += std::string(filters.empty() ? "" : ":") + "stereo_widen";
            if (!set_player_audio_filters(mp_, filters)) WNK_LOGW(kCat, "vlc: audio filters %s not set", filters.c_str());
        }
        if (libvlc_media_player_play(mp_) != 0) error_ = "libVLC refused to play it";
        WNK_LOGI(kCat, "vlc: play %s", loggable(mrl).c_str());
    }

    ~VlcPlayback() override {
        if (mp_ != nullptr) {
            libvlc_media_player_stop(mp_);  // synchronous: no callback runs after this returns
            libvlc_media_player_release(mp_);
        }
    }

    // ---- basics ----
    PlayState state() const override {
        if (!error_.empty() || mp_ == nullptr) return PlayState::Error;
        switch (libvlc_media_player_get_state(mp_)) {
            case libvlc_Playing: return PlayState::Playing;
            case libvlc_Paused: return PlayState::Paused;
            case libvlc_Ended:
            case libvlc_Stopped: return started_ ? PlayState::Ended : PlayState::Opening;
            case libvlc_Error: return PlayState::Error;
            default: return PlayState::Opening;
        }
    }
    int64_t time_ms() const override { return mp_ ? std::max<int64_t>(0, libvlc_media_player_get_time(mp_)) : 0; }
    int64_t length_ms() const override { return mp_ ? std::max<int64_t>(0, libvlc_media_player_get_length(mp_)) : 0; }
    bool seekable() const override { return mp_ && libvlc_media_player_is_seekable(mp_); }
    void set_pause(bool pause) override {
        if (mp_) libvlc_media_player_set_pause(mp_, pause ? 1 : 0);
    }
    void seek_ms(int64_t position) override {
        if (!mp_) return;
        const int64_t len = length_ms();
        if (position < 0) position = 0;
        if (len > 0 && position > len - 1000) position = len - 1000;
        libvlc_media_player_set_time(mp_, position);
    }
    std::shared_ptr<FrameSink> frames() const override { return sink_; }
    std::string describe() const override { return describe_; }
    std::string error() const override {
        if (!error_.empty()) return error_;
        if (mp_ && libvlc_media_player_get_state(mp_) == libvlc_Error) {
            const char *e = libvlc_errmsg();
            return e ? e : "VLC could not play this file";
        }
        return {};
    }
    void tick() override {
        if (!mp_) return;
        const libvlc_state_t s = libvlc_media_player_get_state(mp_);
        if (s == libvlc_Playing || s == libvlc_Paused) started_ = true;
        if (started_ && describe_.empty()) describe_tracks();
        const auto now = std::chrono::steady_clock::now();
        if (s == libvlc_Playing && now >= next_report_) {
            if (next_report_.time_since_epoch().count() != 0) report();
            next_report_ = now + std::chrono::seconds(5);
        }
    }

    // ---- tracks ----
    std::vector<TrackOption> audio_tracks() const override {
        auto v = mp_ ? to_options(libvlc_audio_get_track_description(mp_)) : std::vector<TrackOption>{};
        v.erase(std::remove_if(v.begin(), v.end(), [](const TrackOption &t) { return t.id < 0; }), v.end());
        return v;
    }
    int audio_track() const override { return mp_ ? libvlc_audio_get_track(mp_) : -1; }
    void set_audio_track(int id) override {
        if (mp_) libvlc_audio_set_track(mp_, id);
    }
    std::vector<TrackOption> subtitle_tracks() const override {
        auto v = mp_ ? to_options(libvlc_video_get_spu_description(mp_)) : std::vector<TrackOption>{};
        if (v.empty() || v.front().id != -1) v.insert(v.begin(), TrackOption{-1, "Off"});
        v.front().name = "Off";
        return v;
    }
    int subtitle_track() const override { return mp_ ? libvlc_video_get_spu(mp_) : -1; }
    void set_subtitle_track(int id) override {
        if (mp_) libvlc_video_set_spu(mp_, id);
    }
    bool add_subtitle_file(const std::string &path) override {
        if (!mp_) return false;
        const std::string uri = file_uri(path);
        return libvlc_media_player_add_slave(mp_, libvlc_media_slave_type_subtitle, uri.c_str(), true) == 0;
    }
    std::vector<TrackOption> video_tracks() const override {
        auto v = mp_ ? to_options(libvlc_video_get_track_description(mp_)) : std::vector<TrackOption>{};
        v.erase(std::remove_if(v.begin(), v.end(), [](const TrackOption &t) { return t.id < 0; }), v.end());
        return v;
    }
    int video_track() const override { return mp_ ? libvlc_video_get_track(mp_) : -1; }
    void set_video_track(int id) override {
        if (mp_) libvlc_video_set_track(mp_, id);
    }

    // ---- timing ----
    void set_audio_delay_ms(int64_t ms) override {
        if (mp_) libvlc_audio_set_delay(mp_, ms * 1000);
    }
    void set_subtitle_delay_ms(int64_t ms) override {
        if (mp_) libvlc_video_set_spu_delay(mp_, ms * 1000);
    }
    float rate() const override { return mp_ ? libvlc_media_player_get_rate(mp_) : 1; }
    void set_rate(float r) override {
        if (mp_) libvlc_media_player_set_rate(mp_, r);
    }
    bool can_snapshot() const override { return mp_ != nullptr; }
    bool snapshot(const std::string &png_path) override {
        return mp_ != nullptr && libvlc_video_take_snapshot(mp_, 0, png_path.c_str(), 0, 0) == 0;
    }

    std::vector<Title> titles() const override {
        std::vector<Title> out;
        if (!mp_) return out;
        libvlc_title_description_t **d = nullptr;
        const int n = libvlc_media_player_get_full_title_descriptions(mp_, &d);
        for (int i = 0; i < n; ++i) {
            Title t;
            t.length_ms = d[i]->i_duration;
            t.menu = (d[i]->i_flags & libvlc_title_menu) != 0;
            t.name = d[i]->psz_name && *d[i]->psz_name ? d[i]->psz_name : (t.menu ? "Menu" : "Title " + std::to_string(i + 1));
            out.push_back(std::move(t));
        }
        if (n > 0) libvlc_title_descriptions_release(d, static_cast<unsigned>(n));
        if (out.size() < 2) out.clear();  // a plain file: one title, nothing to choose
        return out;
    }
    int title() const override { return mp_ ? libvlc_media_player_get_title(mp_) : -1; }
    void set_title(int t) override {
        if (mp_) libvlc_media_player_set_title(mp_, t);
    }

    std::vector<Chapter> chapters() const override {
        std::vector<Chapter> out;
        if (!mp_) return out;
        libvlc_chapter_description_t **d = nullptr;
        const int n = libvlc_media_player_get_full_chapter_descriptions(mp_, -1, &d);
        for (int i = 0; i < n; ++i) {
            Chapter c;
            c.start_ms = d[i]->i_time_offset;
            c.name = d[i]->psz_name && *d[i]->psz_name ? d[i]->psz_name : "Chapter " + std::to_string(i + 1);
            out.push_back(std::move(c));
        }
        if (n > 0) libvlc_chapter_descriptions_release(d, static_cast<unsigned>(n));
        return out;
    }
    int chapter() const override { return mp_ ? libvlc_media_player_get_chapter(mp_) : -1; }
    void set_chapter(int c) override {
        if (mp_) libvlc_media_player_set_chapter(mp_, c);
    }

    // ---- audio ----
    std::vector<std::string> equalizer_presets() const override {
        std::vector<std::string> out;
        const unsigned n = libvlc_audio_equalizer_get_preset_count();
        for (unsigned i = 0; i < n; ++i) {
            const char *name = libvlc_audio_equalizer_get_preset_name(i);
            out.emplace_back(name ? name : "Preset");
        }
        return out;
    }
    void set_equalizer(int preset) override {
        AudioEffects fx;
        fx.equalizer = preset;
        apply_equalizer(fx);
    }
    void apply_equalizer(const AudioEffects &fx) {
        if (!mp_) return;
        libvlc_equalizer_t *eq = make_equalizer(fx);
        libvlc_media_player_set_equalizer(mp_, eq);  // libVLC copies it; null: off
        if (eq) libvlc_audio_equalizer_release(eq);
    }
    // Equalizer and volume change live; normalise and widen are filters chosen at open, and with
    // either on, libVLC's equalizer call would drop them: the equalizer then changes by reopening.
    bool apply_audio_effects(const AudioEffects &fx) override {
        if (fx.normalize != opened_.normalize || fx.widen != opened_.widen) return false;
        if ((fx.normalize || fx.widen) && !fx.same_equalizer(opened_)) return false;
        // Only what changed (an unchanged equalizer set again would drop the other filters). The
        // volume always: the audio output may not have existed when it was first set.
        if (!fx.same_equalizer(opened_)) apply_equalizer(fx);
        set_volume_percent(fx.volume);
        opened_ = fx;
        return true;
    }

    void set_volume_percent(int v) override {
        if (mp_) libvlc_audio_set_volume(mp_, std::clamp(v, 0, 200));
    }

    // ---- video ----
    bool can_filter_video() const override { return true; }
    void set_deinterlace(const std::string &mode) override {
        if (mp_) libvlc_video_set_deinterlace(mp_, mode.empty() ? nullptr : mode.c_str());
    }
    void set_picture_adjust(const PictureAdjust &a) override {
        if (!mp_) return;
        libvlc_video_set_adjust_int(mp_, libvlc_adjust_Enable, a.enabled ? 1 : 0);
        if (!a.enabled) return;
        libvlc_video_set_adjust_float(mp_, libvlc_adjust_Brightness, a.brightness);
        libvlc_video_set_adjust_float(mp_, libvlc_adjust_Contrast, a.contrast);
        libvlc_video_set_adjust_float(mp_, libvlc_adjust_Saturation, a.saturation);
        libvlc_video_set_adjust_float(mp_, libvlc_adjust_Gamma, a.gamma);
        libvlc_video_set_adjust_float(mp_, libvlc_adjust_Hue, a.hue);
    }

    // ---- information ----
    std::vector<std::pair<std::string, std::string>> info() const override {
        std::vector<std::pair<std::string, std::string>> rows;
        if (!mp_) return rows;
        libvlc_media_t *m = libvlc_media_player_get_media(mp_);
        if (m == nullptr) return rows;
        char buf[160];
        int v = 0, a = 0, s = 0;
        libvlc_media_track_t **tracks = nullptr;
        const unsigned n = libvlc_media_tracks_get(m, &tracks);
        for (unsigned i = 0; i < n; ++i) {
            const libvlc_media_track_t *t = tracks[i];
            const std::string codec = codec_name(t->i_codec);
            const std::string lang = t->psz_language && *t->psz_language ? std::string(" · ") + t->psz_language : "";
            const std::string kbps = t->i_bitrate > 0 ? " · " + std::to_string(t->i_bitrate / 1000) + " kb/s" : "";
            if (t->i_type == libvlc_track_video && t->video) {
                std::snprintf(buf, sizeof buf, "%s %u×%u", codec.c_str(), t->video->i_width, t->video->i_height);
                std::string val = buf;
                if (t->video->i_frame_rate_den > 0 && t->video->i_frame_rate_num > 0) {
                    std::snprintf(buf, sizeof buf, " · %.3g fps",
                                  static_cast<double>(t->video->i_frame_rate_num) / t->video->i_frame_rate_den);
                    val += buf;
                }
                ++v;
                rows.emplace_back(v > 1 ? "Video " + std::to_string(v) : "Video", val + kbps + lang);
            } else if (t->i_type == libvlc_track_audio && t->audio) {
                std::snprintf(buf, sizeof buf, "%s · %u ch · %u Hz", codec.c_str(), t->audio->i_channels, t->audio->i_rate);
                rows.emplace_back("Audio " + std::to_string(++a), buf + kbps + lang);
            } else if (t->i_type == libvlc_track_text) {
                rows.emplace_back("Subtitles " + std::to_string(++s), codec + lang);
            }
        }
        if (n > 0) libvlc_media_tracks_release(tracks, n);
        libvlc_media_stats_t st{};
        if (libvlc_media_get_stats(m, &st)) {
            std::snprintf(buf, sizeof buf, "%d shown · %d dropped", st.i_displayed_pictures, st.i_lost_pictures);
            rows.emplace_back("Frames", buf);
            std::snprintf(buf, sizeof buf, "%.0f kb/s", st.f_input_bitrate * 8000);
            rows.emplace_back("Input bitrate", buf);
        }
        rows.emplace_back("Decoding", "Software (libVLC)");
        if (char *mrl = libvlc_media_get_mrl(m)) {
            rows.emplace_back("Location", mrl);
            libvlc_free(mrl);
        }
        libvlc_media_release(m);
        return rows;
    }

 private:
    void describe_tracks() {
        libvlc_media_t *m = libvlc_media_player_get_media(mp_);
        if (m == nullptr) return;
        libvlc_media_track_t **tracks = nullptr;
        const unsigned n = libvlc_media_tracks_get(m, &tracks);
        for (unsigned i = 0; i < n; ++i) {
            const libvlc_media_track_t *t = tracks[i];
            if (t->i_type != libvlc_track_video || t->video == nullptr) continue;
            char buf[96];
            std::snprintf(buf, sizeof buf, "%s %u×%u · software", codec_name(t->i_codec).c_str(), t->video->i_width,
                          t->video->i_height);
            describe_ = buf;
            sink_->set_sar(static_cast<int>(t->video->i_sar_num), static_cast<int>(t->video->i_sar_den));
            sink_->set_visible_size(static_cast<int>(t->video->i_width), static_cast<int>(t->video->i_height));
            break;
        }
        if (describe_.empty() && n > 0) describe_ = "Audio";
        if (n > 0) libvlc_media_tracks_release(tracks, n);
        libvlc_media_release(m);
        WNK_LOGI(kCat, "vlc: %s", describe_.c_str());
    }

    // ---- libvlc video callbacks (VLC's video output thread) ----
    void report() {
        libvlc_media_t *m = libvlc_media_player_get_media(mp_);
        libvlc_media_stats_t st{};
        const bool have = m != nullptr && libvlc_media_get_stats(m, &st);
        if (m != nullptr) libvlc_media_release(m);
        const int handed = displayed_.exchange(0);
        const int64_t gap = max_gap_us_.exchange(0);
        const int64_t held = held_us_.exchange(0), held_max = held_max_us_.exchange(0);
        WNK_LOGI(kCat, "vlc: 5 s: %d pictures handed over (longest gap %lld ms; buffer held %.1f ms avg, %.1f max); "
                 "VLC: %d shown, %d lost, %d decoded",
                 handed, static_cast<long long>(gap / 1000), handed > 0 ? held / 1000.0 / handed : 0.0, held_max / 1000.0,
                 have ? st.i_displayed_pictures - last_shown_ : -1,
                 have ? st.i_lost_pictures - last_lost_ : -1, have ? st.i_decoded_video : -1);
        if (have) {
            last_shown_ = st.i_displayed_pictures;
            last_lost_ = st.i_lost_pictures;
        }
    }

    static unsigned setup_cb(void **opaque, char *chroma, unsigned *width, unsigned *height, unsigned *pitches,
                             unsigned *lines) {
        auto *self = static_cast<VlcPlayback *>(*opaque);
        // Full-range 4:2:0 (phones, cameras, MJPEG) is taken as it is: asking for I420 would make VLC
        // convert every picture's range with swscale on its output thread. The shader expands it.
        const bool full = std::memcmp(chroma, "J420", 4) == 0;
        char decoder_chroma[5] = {chroma[0], chroma[1], chroma[2], chroma[3], 0};
        std::memcpy(chroma, full ? "J420" : "I420", 4);
        FrameSink::Format f;
        f.full_range = full;
        f.width = static_cast<int>(*width);
        f.height = static_cast<int>(*height);
        self->sink_->configure(f);
        for (int i = 0; i < 3; ++i) {
            pitches[i] = static_cast<unsigned>(f.pitch[i]);
            lines[i] = static_cast<unsigned>(f.lines[i]);
        }
        WNK_LOGI(kCat, "vlc: video %ux%u, decoder %s, taken as %s", *width, *height, decoder_chroma, full ? "J420" : "I420");
        return 3;
    }
    static void cleanup_cb(void *) {}
    static void *lock_cb(void *opaque, void **planes) {
        auto *self = static_cast<VlcPlayback *>(opaque);
        uint8_t *p[3];
        const int b = self->sink_->begin_write(p);
        for (int i = 0; i < 3; ++i) planes[i] = p[i];
        self->locked_us_.store(std::chrono::duration_cast<std::chrono::microseconds>(
                                   std::chrono::steady_clock::now().time_since_epoch()).count());
        return reinterpret_cast<void *>(static_cast<intptr_t>(b));
    }
    static void unlock_cb(void *, void *, void *const *) {}
    static void display_cb(void *opaque, void *picture) {
        auto *self = static_cast<VlcPlayback *>(opaque);
        self->sink_->end_write(static_cast<int>(reinterpret_cast<intptr_t>(picture)));
        // For the 5-second report: pictures handed over and the longest gap between two.
        const int64_t now = std::chrono::duration_cast<std::chrono::microseconds>(
                                std::chrono::steady_clock::now().time_since_epoch()).count();
        // How long VLC held the buffer (from lock: its copy, conversion and blending into it).
        const int64_t held = now - self->locked_us_.load();
        self->held_us_.fetch_add(held);
        int64_t hm = self->held_max_us_.load();
        while (held > hm && !self->held_max_us_.compare_exchange_weak(hm, held)) {}
        const int64_t last = self->last_display_us_.exchange(now);
        if (last > 0) {
            int64_t gap = now - last, prev = self->max_gap_us_.load();
            while (gap > prev && !self->max_gap_us_.compare_exchange_weak(prev, gap)) {}
        }
        self->displayed_.fetch_add(1);
    }

    libvlc_media_player_t *mp_ = nullptr;
    std::shared_ptr<FrameSink> sink_;
    AudioEffects opened_;
    // Frame-drop diagnosis (RESEARCH.md §10): display callbacks counted on VLC's thread, reported
    // with libVLC's own statistics every 5 seconds while playing.
    std::atomic<int64_t> last_display_us_{0}, max_gap_us_{0};
    std::atomic<int64_t> locked_us_{0}, held_us_{0}, held_max_us_{0};
    std::atomic<int> displayed_{0};
    std::chrono::steady_clock::time_point next_report_{};
    int last_lost_ = 0, last_shown_ = 0;
    std::string error_, describe_;
    bool started_ = false;
};

}  // namespace

std::unique_ptr<Playback> open_vlc_playback(libvlc_instance_t *vlc, const std::string &mrl,
                                            const std::vector<std::string> &options, const AudioEffects &effects) {
    if (vlc == nullptr) return nullptr;
    return std::make_unique<VlcPlayback>(vlc, mrl, options, effects);
}

#else

std::unique_ptr<Playback> open_vlc_playback(libvlc_instance_t *, const std::string &, const std::vector<std::string> &,
                                            const AudioEffects &) {
    return nullptr;
}

#endif

}  // namespace vplayer
