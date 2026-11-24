// SPDX-License-Identifier: GPL-3.0-or-later
//
// VlcSource: a webos-native-app-kit MediaSource whose packets come from libVLC.
//
// libVLC opens and demuxes the media (every container, protocol and playlist VLC knows) and hands
// the selected elementary streams to the app through our "wnk_bridge" stream output
// (modules/wnk_bridge), instead of decoding them. The kit's PlaybackController then feeds them to
// the TV's decoder (Starfish or NDL), which decodes and presents them on the video plane.
//
//   open()    libVLC parses the media: tracks, duration
//   select()  starts libVLC with  #wnk_bridge{sink=...}, or with #transcode{acodec=a52|s16l}
//             in front when the TV cannot take the audio track as it is (kit's audio_path()),
//             and waits for the streams' formats (codec setup, HDR metadata)
//   read()    the next packet: H.264/HEVC Annex-B with parameter sets on keyframes, HDR10+ SEI
//             removed, AAC with ADTS when the backend wants it, AV1 as temporal units
//   seek()    libVLC seeks (fast, from an index point before the target); packets still on their
//             way from before the jump are dropped until VLC flushes each stream; playback then
//             restarts at the newest video keyframe at or before the target
//
// Subtitles: the subtitle track libVLC has selected (by default flags, language preferences, a
// file beside the media, or set_subtitle_track) reaches the bridge too. Text formats
// (subtitle_text.hpp) and PGS, DVD and DVB bitmaps (subtitle_bitmap.hpp) become cues here and are read as Subtitle packets, which the controller keeps in its
// store; they never wait for queue room, so they cannot hold up video or audio. Only the selected
// track is delivered (libVLC selects one subtitle stream at a time), so the subtitle track is
// changed through libVLC, which then adds the new stream.
//
// Back-pressure: the queue holds a few megabytes; when it is full the bridge's send blocks, so
// libVLC reads only as fast as the controller takes packets.
//
// Threading: open/select/read/seek run on the controller's threads; the bridge callbacks run on
// VLC's decoder threads. interrupt() and the destructor unblock both.
#pragma once

#include "player/dovi_detect.hpp"

#include "wnk/media/dovi.hpp"
#include "wnk/media/media.hpp"
#include "wnk/media/time_stretch.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

struct libvlc_instance_t;
struct libvlc_media_player_t;
struct libvlc_media_t;
struct wnk_bridge_sink;
struct wnk_es_desc;

namespace vplayer {

class VlcSource final : public wnk::media::MediaSource {
 public:
    // `options` are extra per-media libVLC options (":network-caching=1000").
    VlcSource(libvlc_instance_t *vlc, std::vector<std::string> options = {});
    ~VlcSource() override;

    bool open(const std::string &uri, std::string *error) override;
    const wnk::media::MediaInfo &info() const override { return info_; }
    bool select(int video_track, int audio_track, const wnk::media::BackendCaps &caps,
                wnk::media::StreamFormat *out, std::string *error) override;
    Read read(wnk::media::Packet *out, std::string *error) override;
    bool seek(int64_t pts_us, std::string *error) override;
    void interrupt() override;
    void reset_interrupt() override;
    // Other speeds from the next select(): audio decoded to stereo PCM by VLC and time-stretched
    // here (wnk::media::TimeStretch), timestamps kept in content time.
    bool set_speed(double speed, std::string *why) override;

    // Audio later (positive) or earlier than the picture, from the next packets read. Any thread.
    void set_audio_delay_us(int64_t us) { audio_delay_us_.store(us); }
    // Always give the TV decoded PCM (Settings: audio "Decoded"), from the next select().
    void set_force_pcm(bool on) { force_pcm_.store(on); }
    // Every SubRip line as an ASS event too (Settings: a subtitle colour other than white).
    void set_subrip_all_ass(bool on) { subrip_all_ass_.store(on); }

    std::string audio_path_text() const;  // "passthrough", "re-encoded to AC-3", "decoded to PCM"
    // How Dolby Vision is played ("Profile 8.1 (configuration record)"), empty when it is not.
    std::string dovi_text() const;
    // Fonts attached to the file (local Matroska with an ASS/SSA track), read at open().
    std::vector<wnk::media::FontAttachment> fonts() const;

    // Subtitles, from the main thread (libVLC's player, which select() and seek() may replace on the
    // controller's threads, is held under its own lock while used).
    struct SubtitleTrack {
        int id = -1;  // VLC's es id; -1 is "off"
        std::string name;
        wnk::media::SubtitleKind kind = wnk::media::SubtitleKind::None;
    };
    std::vector<SubtitleTrack> subtitle_tracks() const;  // "off" first, as libVLC lists them
    int subtitle_track() const;                          // libVLC's selection, -1 for none
    void set_subtitle_track(int id);
    bool add_subtitle_file(const std::string &path);     // selected once libVLC has read it
    // The ASS/SSA script header of a subtitle stream libVLC has delivered (empty otherwise), for
    // styled rendering.
    std::string ass_header(int id) const;
    // The codec label of a delivered subtitle stream ("subt", "ssa", "pgs"), empty if not seen yet.
    std::string subtitle_codec(int id) const;

    // Chapters of the current title as libVLC reads them (Matroska, MP4, DVD...), in content time;
    // empty until the input has started or when the media has none. Main thread.
    struct ChapterMark {
        std::string name;
        int64_t start_ms = 0;
    };
    std::vector<ChapterMark> chapters() const;

 private:
    struct Stream;
    struct Item {
        Stream *stream = nullptr;
        std::vector<uint8_t> data;
        int64_t pts = wnk::media::kNoTime, dts = wnk::media::kNoTime, duration = 0;
        bool key = false;
        std::shared_ptr<const wnk::media::SubtitleCue> cue;  // subtitle streams: the decoded cue
    };
    struct SeenSubtitle {
        uint32_t codec = 0;
        std::string ass_header;
    };

    // Bridge callbacks (VLC threads).
    static void *on_add(void *opaque, const wnk_es_desc *desc);
    static void on_del(void *opaque, void *id);
    static int on_send(void *opaque, void *id, const uint8_t *data, size_t size, int64_t pts, int64_t dts,
                       int64_t length, unsigned flags);
    static void on_flush(void *opaque, void *id);

    bool start_player(std::string *error);
    void stop_player();
    std::string sout_chain() const;
    bool to_packet(Item &item, wnk::media::Packet *out);  // false: nothing to read from this item
    bool stretch_audio(wnk::media::Packet *p);

    libvlc_instance_t *vlc_;
    std::vector<std::string> options_;
    std::unique_ptr<wnk_bridge_sink> sink_;
    std::string uri_;
    libvlc_media_t *media_ = nullptr;
    // Replaced only by the controller's threads (start_player, stop_player); read by the main
    // thread's subtitle calls through hold_player(), under player_mu_.
    libvlc_media_player_t *mp_ = nullptr;
    mutable std::mutex player_mu_;
    libvlc_media_player_t *hold_player() const;  // retained, or nullptr; release when done
    std::vector<std::string> subtitle_files_;    // added files, given to every new player (player_mu_)
    mutable std::atomic<int> last_subtitle_{-1};
    mutable std::vector<ChapterMark> chapters_;  // cached once known (player_mu_)  // libVLC's selection while its input was live
    int subtitle_choice_ = -2;                   // the viewer's last choice (-2: none yet) (player_mu_)
    wnk::media::MediaInfo info_;
    wnk::media::BackendCaps caps_;
    wnk::media::AudioPath audio_path_ = wnk::media::AudioPath::None;
    int want_video_ = -1, want_audio_ = -1;  // TrackInfo indexes (VLC es ids)
    int pcm_rate_ = 48000, pcm_channels_ = 2, ac3_channels_ = 6;
    DoviConfig dovi_record_;            // from the file's header (open)
    bool dovi_convert_ = false;         // Profile 7: strip the enhancement layer, RPUs to 8.1 (reader thread)
    wnk::media::DoviStats dovi_stats_;  // reader thread
    std::string dovi_text_;             // mu_
    std::vector<wnk::media::FontAttachment> fonts_;  // mu_
    std::atomic<double> speed_{1.0};
    std::atomic<int64_t> audio_delay_us_{0};
    std::atomic<bool> force_pcm_{false};
    std::atomic<bool> subrip_all_ass_{false};
    // Reader thread (select() runs while it is parked): the stretcher and its timeline.
    std::unique_ptr<wnk::media::TimeStretch> stretch_;
    int64_t stretch_base_us_ = wnk::media::kNoTime, stretch_in_us_ = wnk::media::kNoTime, stretch_frames_ = 0;
    std::vector<float> stretch_in_, stretch_out_;

    mutable std::mutex mu_;
    std::condition_variable cv_;       // queue has packets / streams arrived / state changed
    std::condition_variable space_;    // queue has room
    std::vector<std::unique_ptr<Stream>> streams_;
    Stream *video_ = nullptr, *audio_ = nullptr;
    std::map<int, SeenSubtitle> seen_subtitles_;  // by es id, kept across restarts of the player
    std::deque<Item> queue_;
    size_t queued_bytes_ = 0;
    bool stopping_ = false, interrupted_ = false;
    bool started_ = false;             // the player reached Playing at least once
    // Timestamps are rebased when a stream starts far from zero (MPEG-TS): content time then
    // matches libVLC's time, which seek() uses.
    bool base_known_ = false;
    int64_t base_us_ = 0;
    // After a seek: drop packets until a video keyframe near the target (or audio, without video).
    bool seeking_ = false;
    int64_t seek_target_us_ = 0;
    std::chrono::steady_clock::time_point seek_started_;
    std::deque<Item> seek_gop_;  // from the newest keyframe at or before the target (mu_)
    size_t seek_gop_bytes_ = 0;
    int64_t seek_gop_key_us_ = 0;
    void land_seek_locked(int64_t at);
    void commit_seek_gop_locked();
    int64_t audio_from_us_ = wnk::media::kNoTime;
};

}  // namespace vplayer
