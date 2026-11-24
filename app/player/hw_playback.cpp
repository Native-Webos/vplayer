// SPDX-License-Identifier: GPL-3.0-or-later
#include "player/hw_playback.hpp"

#include "player/vlc_source.hpp"

#include "wnk/log.hpp"
#include "wnk/media/auto_backend.hpp"
#include "wnk/media/controller.hpp"
#include "wnk/media/ndl_backend.hpp"
#include "wnk/media/starfish_backend.hpp"

#include <cstdio>

namespace vplayer {

namespace media = wnk::media;

namespace {

constexpr const char *kCat = "hwplay";

std::unique_ptr<media::MediaBackend> make_backend(const HwOptions &o) {
    std::string why_sf, why_ndl;
    const bool starfish = media::StarfishBackend::available(&why_sf);
    const bool ndl = media::NdlBackend::available(&why_ndl);
    media::StarfishOptions so;
    so.dolby_vision = o.dolby_vision;
    so.atmos = o.atmos;
    if (starfish && ndl) {
        return std::make_unique<media::AutoBackend>(std::make_unique<media::StarfishBackend>(so),
                                                    std::make_unique<media::NdlBackend>());
    }
    if (starfish) return std::make_unique<media::StarfishBackend>(so);
    if (ndl) return std::make_unique<media::NdlBackend>();
    return nullptr;
}

std::string codec_text(media::CodecId c) { return media::codec_name(c); }

std::string track_name(const media::TrackInfo &t) {
    std::string n = t.title;
    if (!t.language.empty()) n += (n.empty() ? "" : " · ") + t.language;
    std::string tech = t.codec != media::CodecId::Unknown ? codec_text(t.codec) : t.codec_label;
    if (t.channels > 0) tech += " " + std::to_string(t.channels) + " ch";
    n += (n.empty() ? "" : " · ") + tech;
    return n;
}

class HwPlayback final : public Playback {
 public:
    HwPlayback(libvlc_instance_t *vlc, const std::string &path, wnk::async::MainDispatcher &main,
               const HwOptions &options, std::vector<std::string> vlc_options) {
        auto backend = make_backend(options);
        if (!backend) {
            error_ = "no hardware decoder";
            return;
        }
        backend_name_ = backend->name();
        media::PlaybackOptions po;
        po.autoplay = true;
        controller_ = std::make_unique<media::PlaybackController>(
            std::make_unique<VlcSource>(vlc, std::move(vlc_options)), std::move(backend), main, po);
        controller_->set_state_callback([this](media::PlayState from, media::PlayState to,
                                               const media::PlaybackSnapshot &snap) {
            WNK_LOGI(kCat, "%s -> %s%s%s", media::play_state_name(from), media::play_state_name(to),
                     snap.error.empty() ? "" : ": ", snap.error.c_str());
            if (to == media::PlayState::Ready) controller_->set_video_area({0, 0, 1920, 1080});
            if (to == media::PlayState::Playing) played_ = true;
        });
        controller_->set_video_area({0, 0, 1920, 1080});
        controller_->open(path);
        WNK_LOGI(kCat, "open %s with %s", path.c_str(), backend_name_.c_str());
    }

    ~HwPlayback() override {
        if (controller_) controller_->stop();
    }

    PlayState state() const override {
        if (!controller_) return PlayState::Error;
        switch (controller_->state()) {
            case media::PlayState::Playing: return PlayState::Playing;
            case media::PlayState::Paused: return PlayState::Paused;
            case media::PlayState::Seeking: return paused_ ? PlayState::Paused : PlayState::Playing;
            case media::PlayState::Ended:
            case media::PlayState::Stopped: return PlayState::Ended;
            case media::PlayState::Error: return PlayState::Error;
            default: return PlayState::Opening;
        }
    }
    int64_t time_ms() const override {
        const auto s = controller_ ? controller_->snapshot() : media::PlaybackSnapshot{};
        return s.position_us == media::kNoTime ? 0 : s.position_us / 1000;
    }
    int64_t length_ms() const override {
        const auto s = controller_ ? controller_->snapshot() : media::PlaybackSnapshot{};
        return s.duration_us == media::kNoTime ? 0 : s.duration_us / 1000;
    }
    bool seekable() const override { return controller_ && controller_->snapshot().info.seekable; }
    void set_pause(bool pause) override {
        if (!controller_) return;
        paused_ = pause;
        if (pause) {
            controller_->pause();
        } else {
            controller_->play();
        }
    }
    void seek_ms(int64_t position) override {
        if (!controller_) return;
        const int64_t len = length_ms();
        if (position < 0) position = 0;
        if (len > 0 && position > len - 1000) position = len - 1000;
        controller_->seek(position * 1000);
    }
    std::shared_ptr<FrameSink> frames() const override { return nullptr; }
    bool video_plane() const override { return true; }
    bool can_fall_back() const override { return !played_ && state() == PlayState::Error; }

    std::string describe() const override {
        if (!controller_) return {};
        const auto s = controller_->snapshot();
        if (!s.format.video && !s.format.audio) return {};
        std::string d;
        if (s.format.video) {
            char b[64];
            std::snprintf(b, sizeof b, "%s %d×%d", codec_text(s.format.video->codec).c_str(), s.format.video->width,
                          s.format.video->height);
            d = b;
            const int t = s.format.video->colour.transfer;
            if (s.format.video->dolby_vision_profile >= 0) {
                d += " · Dolby Vision";
            } else if (t == 16) {
                d += " · HDR10";
            } else if (t == 18) {
                d += " · HLG";
            }
        } else {
            d = codec_text(s.format.audio->codec);
        }
        return d + " · TV decoder";
    }
    std::string error() const override {
        if (!error_.empty()) return error_;
        return controller_ ? controller_->snapshot().error : std::string();
    }

    std::vector<TrackOption> audio_tracks() const override {
        std::vector<TrackOption> out;
        if (!controller_) return out;
        for (const auto &t : controller_->snapshot().info.tracks)
            if (t.type == media::TrackType::Audio) out.push_back({t.index, track_name(t)});
        return out;
    }
    int audio_track() const override { return controller_ ? controller_->snapshot().audio_track : -1; }
    void set_audio_track(int id) override {
        if (controller_) controller_->set_audio_track(id);
    }
    std::vector<TrackOption> subtitle_tracks() const override {
        return {{-1, "Off (subtitles on the TV decoder are coming)"}};
    }
    std::vector<TrackOption> video_tracks() const override {
        std::vector<TrackOption> out;
        if (!controller_) return out;
        for (const auto &t : controller_->snapshot().info.tracks)
            if (t.type == media::TrackType::Video) out.push_back({t.index, track_name(t)});
        return out;
    }
    int video_track() const override { return controller_ ? controller_->snapshot().video_track : -1; }
    float rate() const override { return controller_ ? static_cast<float>(controller_->snapshot().speed) : 1; }
    void set_rate(float r) override {
        if (controller_) controller_->set_speed(r);
    }

    std::vector<std::pair<std::string, std::string>> info() const override {
        std::vector<std::pair<std::string, std::string>> rows;
        if (!controller_) return rows;
        const auto s = controller_->snapshot();
        for (const auto &t : s.info.tracks) {
            if (t.type == media::TrackType::Video && t.index == s.video_track) {
                char b[96];
                std::snprintf(b, sizeof b, "%s %d×%d · %.3g fps", t.codec_label.c_str(), t.width, t.height, t.frame_rate);
                rows.emplace_back("Video", b);
            } else if (t.type == media::TrackType::Audio && t.index == s.audio_track) {
                rows.emplace_back("Audio", track_name(t) + " · " + std::to_string(t.sample_rate) + " Hz");
            }
        }
        if (s.format.audio) {
            const bool same = [&] {
                for (const auto &t : s.info.tracks)
                    if (t.index == s.audio_track) return t.codec == s.format.audio->codec;
                return true;
            }();
            rows.emplace_back("To the TV", codec_text(s.format.audio->codec) + " · " +
                                               std::to_string(s.format.audio->channels) + " ch" +
                                               (same ? "" : " (converted by VLC)"));
        }
        rows.emplace_back("Decoding", std::string("TV decoder (") + controller_->backend_name() + ")");
        rows.emplace_back("Packets fed", std::to_string(s.video_packets) + " video · " + std::to_string(s.audio_packets) + " audio");
        rows.emplace_back("Rebuffers", std::to_string(s.rebuffers));
        return rows;
    }

 private:
    std::unique_ptr<media::PlaybackController> controller_;
    std::string backend_name_, error_;
    bool paused_ = false;
    bool played_ = false;  // set on the main thread by the state callback
};

}  // namespace

bool hw_playback_available(std::string *why) {
    static int cached = -1;
    static std::string cached_why;
    if (cached < 0) {
        std::string a, b;
        const bool ok = media::StarfishBackend::available(&a) || media::NdlBackend::available(&b);
        cached = ok ? 1 : 0;
        cached_why = ok ? "" : "Starfish: " + a + "; NDL: " + b;
    }
    if (why) *why = cached_why;
    return cached == 1;
}

std::unique_ptr<Playback> open_hw_playback(libvlc_instance_t *vlc, const std::string &path,
                                           wnk::async::MainDispatcher &main, const HwOptions &options,
                                           std::vector<std::string> vlc_options) {
    if (vlc == nullptr) return nullptr;
    return std::make_unique<HwPlayback>(vlc, path, main, options, std::move(vlc_options));
}

}  // namespace vplayer
