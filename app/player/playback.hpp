// SPDX-License-Identifier: GPL-3.0-or-later
//
// Playback seen from the player screen: a state, a clock, a few controls, and decoded video frames.
//
// Two implementations:
//   * VLC (software path, P1): libVLC decodes; frames arrive in I420 through libvlc's video callbacks
//     and are drawn by VideoView with a YUV shader; audio goes to PulseAudio by libVLC itself.
//   * Demo: a test pattern and a fake clock, for host builds without libVLC (UI work, screenshots).
//   * Hardware (hw_playback.hpp): the TV decodes onto its video plane; subtitles come back as cues
//     the screen draws (subtitle_cues), since VLC neither decodes nor blends them there.
//
// Threading: every method is called on the main thread. FrameSink is the one object shared with a
// decoder thread.
#pragma once

#include "wnk/media/media.hpp"

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

struct libvlc_instance_t;

namespace vplayer {

// Decoded I420 frames handed from a decoder thread to the main thread, three buffers deep: the writer
// fills one, one is the newest finished frame, the main thread reads the third. Nobody waits: the
// writer overwrites an unread finished frame, the reader keeps the last one until a newer exists.
class FrameSink {
 public:
    struct Format {
        int width = 0, height = 0;       // visible picture
        int pitch[3] = {0, 0, 0};        // bytes per row of Y, U, V
        int lines[3] = {0, 0, 0};        // rows allocated per plane
        int sar_num = 1, sar_den = 1;    // sample aspect ratio (non-square pixels)
        bool full_range = false;         // J420 (0-255) rather than I420 (16-235)
        // The part of width×height that is picture: libVLC's video callbacks are given the coded size
        // (1280×738 for a 1280×720 H.264 stream), so the visible size comes from the track (0: all).
        int visible_width = 0, visible_height = 0;
    };

    // Decoder side ------------------------------------------------------------------------------------
    // (Re)allocates the buffers for a picture size; fills fmt's pitches and lines.
    void configure(Format &fmt);
    // A buffer to decode into (its three plane pointers). Never the one the main thread reads.
    int begin_write(uint8_t *planes[3]);
    void end_write(int buffer);  // the frame is complete and the newest
    void set_sar(int num, int den);
    void set_visible_size(int width, int height);

    // Main thread -------------------------------------------------------------------------------------
    // The newest finished frame if it is newer than the last one taken; it stays valid (unchanged)
    // until the next call. False if there is nothing new.
    bool take(Format *fmt, const uint8_t *planes[3], uint64_t *serial);
    Format format() const;
    // Bumps with every finished frame: lets the main thread redraw only when there is a new one.
    uint64_t serial() const;

 private:
    mutable std::mutex mu_;
    Format fmt_;
    std::vector<uint8_t> buf_[3];
    size_t plane_offset_[3] = {0, 0, 0};
    int writing_ = -1, ready_ = -1, reading_ = -1;
    uint64_t serial_ = 0;       // bumps with every finished frame
    uint64_t generation_ = 0;   // bumps with every configure
};

enum class PlayState : uint8_t { Opening, Playing, Paused, Ended, Error };

// A selectable track as libVLC names it ("Track 1 - [English]"). id -1 is "off" where that exists.
struct TrackOption {
    int id = -1;
    std::string name;
};

struct Chapter {
    std::string name;
    int64_t start_ms = 0;
};

// Picture adjustments (VLC's "adjust" filter). Neutral values: 1, 1, 1, 1, 0.
struct PictureAdjust {
    bool enabled = false;
    float brightness = 1, contrast = 1, saturation = 1, gamma = 1, hue = 0;
};

// Everything the player's panel can change. Implementations ignore what they cannot do; the
// defaults below are "not supported" (empty lists, no-ops).
class Playback {
 public:
    virtual ~Playback() = default;
    virtual PlayState state() const = 0;
    virtual int64_t time_ms() const = 0;
    virtual int64_t length_ms() const = 0;  // 0 while unknown
    virtual bool seekable() const = 0;
    virtual void set_pause(bool pause) = 0;
    virtual void seek_ms(int64_t position) = 0;  // absolute, clamped
    virtual std::shared_ptr<FrameSink> frames() const = 0;
    // What is playing, for the format chip ("H.264 1280×720 · software"); empty until known.
    virtual std::string describe() const = 0;
    virtual std::string error() const = 0;
    // Called every frame on the main thread: lets an implementation poll its engine.
    virtual void tick() {}
    // The TV draws the picture on its video plane under the UI (hardware path): the screen must
    // be cleared to transparent and frames() is unused.
    virtual bool video_plane() const { return false; }
    // Failed before showing anything in a way another engine may not (the TV decoder refused the
    // stream): the player should try the software path.
    virtual bool can_fall_back() const { return false; }

    // ---- tracks ----
    virtual std::vector<TrackOption> audio_tracks() const { return {}; }
    virtual int audio_track() const { return -1; }
    virtual void set_audio_track(int) {}
    virtual std::vector<TrackOption> subtitle_tracks() const { return {}; }  // includes "off" (-1)
    virtual int subtitle_track() const { return -1; }
    virtual void set_subtitle_track(int) {}
    virtual bool add_subtitle_file(const std::string &) { return false; }
    virtual std::vector<TrackOption> video_tracks() const { return {}; }
    virtual int video_track() const { return -1; }
    virtual void set_video_track(int) {}

    // ---- subtitles the screen draws (hardware path) ----
    // The cues of the selected subtitle track showing now, with the subtitle delay applied. Empty
    // when the engine draws subtitles into its own frames (software path) or none are showing.
    virtual std::vector<std::shared_ptr<const wnk::media::SubtitleCue>> subtitle_cues() const { return {}; }
    // ASS/SSA: the selected track's script header (empty: plain text) and the picture's size, which
    // the script's coordinates are scaled from.
    virtual std::string subtitle_ass_header() const { return {}; }
    // Fonts the media carries for its styled subtitles (Matroska attachments).
    virtual std::vector<wnk::media::FontAttachment> subtitle_fonts() const { return {}; }
    virtual std::pair<int, int> video_size() const { return {0, 0}; }
    // How text subtitles should look (Settings): height as a fraction of the picture, on a box.
    virtual void set_subtitle_look(float scale, bool box) {
        (void)scale;
        (void)box;
    }

    // ---- timing ----
    virtual void set_audio_delay_ms(int64_t) {}
    virtual void set_subtitle_delay_ms(int64_t) {}
    virtual float rate() const { return 1; }
    // False where set_rate does nothing (the TV decoder path, until its audio can be stretched).
    virtual bool can_change_rate() const { return true; }
    virtual void set_rate(float) {}
    virtual std::vector<Chapter> chapters() const { return {}; }
    virtual int chapter() const { return -1; }
    virtual void set_chapter(int) {}

    // ---- audio ----
    virtual std::vector<std::string> equalizer_presets() const { return {}; }
    virtual void set_equalizer(int /*preset, -1 off*/) {}
    virtual void set_volume_percent(int) {}  // 100 = unchanged, up to 200

    // ---- video ----
    // VLC's deinterlace modes ("yadif", "bob", ...); empty turns deinterlacing off.
    virtual bool can_filter_video() const { return false; }
    virtual void set_deinterlace(const std::string &) {}
    virtual void set_picture_adjust(const PictureAdjust &) {}

    // ---- information ----
    // Label/value pairs for the Info tab ("Video", "H.264 1920×1080 · 23.976 fps").
    virtual std::vector<std::pair<std::string, std::string>> info() const { return {}; }
};

// libVLC software playback of a local path or URL; nullptr if libVLC is not in this build.
// `options` are per-media libVLC options (":no-audio", ":avcodec-threads=4"), for experiments.
std::unique_ptr<Playback> open_vlc_playback(libvlc_instance_t *vlc, const std::string &mrl,
                                            const std::vector<std::string> &options = {});
// A test pattern with a two-hour fake clock.
std::unique_ptr<Playback> open_demo_playback();

}  // namespace vplayer
