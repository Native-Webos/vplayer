// SPDX-License-Identifier: GPL-3.0-or-later
//
// Demo playback for host builds: SMPTE-like colour bars in I420 and a fake two-hour clock, so the
// player screen can be developed and screenshotted without libVLC.
#include "player/playback.hpp"

#include <chrono>

namespace vplayer {

namespace {

using Clock = std::chrono::steady_clock;

class DemoPlayback final : public Playback {
 public:
    DemoPlayback() : sink_(std::make_shared<FrameSink>()), start_(Clock::now()) {
        FrameSink::Format f;
        f.width = 1280;
        f.height = 720;
        sink_->configure(f);
        // Bars: white, yellow, cyan, green, magenta, red, blue (BT.709 limited-range YUV).
        static const uint8_t bars[7][3] = {{235, 128, 128}, {219, 16, 138}, {188, 154, 16}, {173, 42, 26},
                                           {78, 214, 230},  {63, 102, 240}, {32, 240, 118}};
        uint8_t *p[3];
        const int b = sink_->begin_write(p);
        for (int y = 0; y < f.height; ++y)
            for (int x = 0; x < f.width; ++x) p[0][y * f.pitch[0] + x] = bars[x * 7 / f.width][0];
        for (int y = 0; y < f.height / 2; ++y)
            for (int x = 0; x < f.width / 2; ++x) {
                const auto &c = bars[x * 2 * 7 / f.width];
                p[1][y * f.pitch[1] + x] = c[1];
                p[2][y * f.pitch[2] + x] = c[2];
            }
        sink_->end_write(b);
    }

    PlayState state() const override { return paused_ ? PlayState::Paused : PlayState::Playing; }
    int64_t time_ms() const override {
        const int64_t t = paused_ ? paused_at_
                                  : offset_ + std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start_).count();
        return t < kLength ? t : kLength;
    }
    int64_t length_ms() const override { return kLength; }
    bool seekable() const override { return true; }
    void set_pause(bool pause) override {
        if (pause == paused_) return;
        if (pause) {
            paused_at_ = time_ms();
        } else {
            offset_ = paused_at_;
            start_ = Clock::now();
        }
        paused_ = pause;
    }
    void seek_ms(int64_t position) override {
        position = position < 0 ? 0 : (position > kLength ? kLength : position);
        offset_ = paused_at_ = position;
        start_ = Clock::now();
    }
    std::shared_ptr<FrameSink> frames() const override { return sink_; }
    std::string describe() const override { return "Test pattern 1280×720 · demo"; }
    std::string error() const override { return {}; }

    // Fake tracks and chapters, so the player's panel can be built and screenshotted on a desktop.
    std::vector<TrackOption> audio_tracks() const override {
        return {{1, "English · E-AC-3 5.1"}, {2, "Commentary · AAC stereo"}};
    }
    int audio_track() const override { return audio_; }
    void set_audio_track(int id) override { audio_ = id; }
    std::vector<TrackOption> subtitle_tracks() const override {
        return {{-1, "Off"}, {3, "English · SRT (file next to the video)"}, {4, "English SDH · PGS"}, {5, "Spanish · ASS"}};
    }
    int subtitle_track() const override { return spu_; }
    void set_subtitle_track(int id) override { spu_ = id; }
    float rate() const override { return rate_; }
    void set_rate(float r) override { rate_ = r; }
    std::vector<Chapter> chapters() const override {
        return {{"Opening", 0}, {"The road north", 900000}, {"Base camp", 2400000}, {"Night market", 5000000},
                {"The summit", 6600000}, {"Credits", 7800000}};
    }
    int chapter() const override {
        int c = 0;
        const auto ch = chapters();
        for (size_t i = 0; i < ch.size(); ++i)
            if (time_ms() >= ch[i].start_ms) c = static_cast<int>(i);
        return c;
    }
    void set_chapter(int c) override {
        const auto ch = chapters();
        if (c >= 0 && c < static_cast<int>(ch.size())) seek_ms(ch[static_cast<size_t>(c)].start_ms);
    }
    std::vector<std::string> equalizer_presets() const override {
        return {"Flat", "Classical", "Club", "Dance", "Full bass", "Full treble", "Headphones", "Large Hall",
                "Live", "Party", "Pop", "Reggae", "Rock", "Ska", "Soft", "Soft rock", "Techno"};
    }
    bool can_filter_video() const override { return true; }
    std::vector<std::pair<std::string, std::string>> info() const override {
        return {{"Video", "Test pattern 1280×720 · 30 fps"},
                {"Audio 1", "E-AC-3 · 6 ch · 48000 Hz · English"},
                {"Audio 2", "AAC · 2 ch · 48000 Hz · English"},
                {"Subtitles 1", "SubRip · English"},
                {"Frames", "2512 shown · 0 dropped"},
                {"Decoding", "Demo engine (host build)"}};
    }

 private:
    int audio_ = 1, spu_ = 3;
    float rate_ = 1;
    static constexpr int64_t kLength = 2 * 3600 * 1000 + 14 * 60 * 1000 + 5 * 1000;
    std::shared_ptr<FrameSink> sink_;
    Clock::time_point start_;
    int64_t offset_ = 5021 * 1000, paused_at_ = 0;
    bool paused_ = false;
};

}  // namespace

std::unique_ptr<Playback> open_demo_playback() { return std::make_unique<DemoPlayback>(); }

}  // namespace vplayer
