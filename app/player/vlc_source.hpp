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
//   seek()    libVLC seeks (fast, to a keyframe); packets from before the jump are dropped until
//             a video keyframe near the target arrives
//
// Back-pressure: the queue holds a few megabytes; when it is full the bridge's send blocks, so
// libVLC reads only as fast as the controller takes packets.
//
// Threading: open/select/read/seek run on the controller's threads; the bridge callbacks run on
// VLC's decoder threads. interrupt() and the destructor unblock both.
#pragma once

#include "wnk/media/media.hpp"

#include <condition_variable>
#include <cstdint>
#include <deque>
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

    // For the player's panel and info (any thread; libVLC's calls are thread-safe).
    libvlc_media_player_t *player() const { return mp_; }
    std::string audio_path_text() const;  // "passthrough", "re-encoded to AC-3", "decoded to PCM"

 private:
    struct Stream;
    struct Item {
        Stream *stream = nullptr;
        std::vector<uint8_t> data;
        int64_t pts = wnk::media::kNoTime, dts = wnk::media::kNoTime, duration = 0;
        bool key = false;
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
    void to_packet(Item &item, wnk::media::Packet *out);

    libvlc_instance_t *vlc_;
    std::vector<std::string> options_;
    std::unique_ptr<wnk_bridge_sink> sink_;
    std::string uri_;
    libvlc_media_t *media_ = nullptr;
    libvlc_media_player_t *mp_ = nullptr;
    wnk::media::MediaInfo info_;
    wnk::media::BackendCaps caps_;
    wnk::media::AudioPath audio_path_ = wnk::media::AudioPath::None;
    int want_video_ = -1, want_audio_ = -1;  // TrackInfo indexes (VLC es ids)
    int pcm_rate_ = 48000, pcm_channels_ = 2, ac3_channels_ = 6;

    mutable std::mutex mu_;
    std::condition_variable cv_;       // queue has packets / streams arrived / state changed
    std::condition_variable space_;    // queue has room
    std::vector<std::unique_ptr<Stream>> streams_;
    Stream *video_ = nullptr, *audio_ = nullptr;
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
    int64_t audio_from_us_ = wnk::media::kNoTime;
};

}  // namespace vplayer
