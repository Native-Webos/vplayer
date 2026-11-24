// SPDX-License-Identifier: GPL-3.0-or-later
#include "player/vlc_source.hpp"

#include "wnk_bridge/wnk_bridge.h"

#include "wnk/log.hpp"
#include "wnk/media/bitstream.hpp"

#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cstring>
#include <thread>

#if VPLAYER_HAVE_LIBVLC
#include <vlc/vlc.h>
#endif

namespace vplayer {

namespace media = wnk::media;
using media::CodecId;
using media::kNoTime;

namespace {

constexpr const char *kCat = "vlcsrc";
constexpr size_t kMaxQueueBytes = 6u << 20;  // a few seconds of 4K HEVC; the controller reads ahead itself
constexpr size_t kMaxQueueItems = 400;
constexpr int64_t kSeekWindowUs = 20'000'000;  // a keyframe this close to the target ends a seek
constexpr int64_t kRebaseAboveUs = 1'000'000;  // streams starting later than this are rebased

constexpr uint32_t fourcc(char a, char b, char c, char d) {
    return static_cast<uint32_t>(static_cast<uint8_t>(a)) | static_cast<uint32_t>(static_cast<uint8_t>(b)) << 8 |
           static_cast<uint32_t>(static_cast<uint8_t>(c)) << 16 | static_cast<uint32_t>(static_cast<uint8_t>(d)) << 24;
}

CodecId codec_of(uint32_t f) {
    switch (f) {
        case fourcc('h', '2', '6', '4'): return CodecId::H264;
        case fourcc('h', 'e', 'v', 'c'): return CodecId::Hevc;
        case fourcc('V', 'P', '9', '0'): return CodecId::Vp9;
        case fourcc('a', 'v', '0', '1'): return CodecId::Av1;
        case fourcc('m', 'p', '4', 'a'): return CodecId::Aac;
        case fourcc('a', '5', '2', ' '): return CodecId::Ac3;
        case fourcc('e', 'a', 'c', '3'): return CodecId::Eac3;
        case fourcc('m', 'p', 'g', 'a'): return CodecId::Mp3;
        case fourcc('m', 'p', '3', ' '): return CodecId::Mp3;
        case fourcc('o', 'p', 'u', 's'): return CodecId::Opus;
        case fourcc('s', '1', '6', 'l'): return CodecId::PcmS16;
        default: return CodecId::Unknown;
    }
}

std::string fourcc_text(uint32_t f) {
    std::string s;
    for (int i = 0; i < 4; ++i) {
        const char c = static_cast<char>((f >> (8 * i)) & 0xFF);
        if (c != ' ' && c != 0) s += c;
    }
    return s;
}

bool is_annexb(const std::vector<uint8_t> &d) {
    return (d.size() >= 4 && d[0] == 0 && d[1] == 0 && d[2] == 0 && d[3] == 1) ||
           (d.size() >= 3 && d[0] == 0 && d[1] == 0 && d[2] == 1);
}

// avcC / hvcC (MP4/MKV codec private data) → Annex-B parameter sets. Empty if it is neither.
std::vector<uint8_t> config_to_annexb(const std::vector<uint8_t> &c, bool hevc) {
    std::vector<uint8_t> out;
    auto put = [&out](const uint8_t *p, size_t n) {
        static const uint8_t sc[4] = {0, 0, 0, 1};
        out.insert(out.end(), sc, sc + 4);
        out.insert(out.end(), p, p + n);
    };
    if (!hevc) {
        if (c.size() < 7 || c[0] != 1) return {};
        size_t i = 5;
        int n = c[i++] & 0x1F;  // SPS
        for (int k = 0; k < n; ++k) {
            if (i + 2 > c.size()) return {};
            const size_t len = static_cast<size_t>(c[i] << 8 | c[i + 1]);
            i += 2;
            if (i + len > c.size()) return {};
            put(&c[i], len);
            i += len;
        }
        if (i >= c.size()) return out;
        n = c[i++];  // PPS
        for (int k = 0; k < n; ++k) {
            if (i + 2 > c.size()) return out;
            const size_t len = static_cast<size_t>(c[i] << 8 | c[i + 1]);
            i += 2;
            if (i + len > c.size()) return out;
            put(&c[i], len);
            i += len;
        }
        return out;
    }
    if (c.size() < 23 || c[0] != 1) return {};
    size_t i = 22;
    const int arrays = c[i++];
    for (int a = 0; a < arrays; ++a) {
        if (i + 3 > c.size()) return out;
        i += 1;  // array_completeness, nal type
        const int n = c[i] << 8 | c[i + 1];
        i += 2;
        for (int k = 0; k < n; ++k) {
            if (i + 2 > c.size()) return out;
            const size_t len = static_cast<size_t>(c[i] << 8 | c[i + 1]);
            i += 2;
            if (i + len > c.size()) return out;
            put(&c[i], len);
            i += len;
        }
    }
    return out;
}

// Whether an Annex-B access unit carries its own sequence parameter set.
bool has_sps(const std::vector<uint8_t> &d, bool hevc) {
    for (size_t i = 0; i + 3 < d.size(); ++i) {
        if (d[i] != 0 || d[i + 1] != 0 || d[i + 2] != 1) continue;
        const uint8_t h = d[i + 3];
        if (!hevc && (h & 0x1F) == 7) return true;
        if (hevc && ((h >> 1) & 0x3F) == 33) return true;
        i += 2;
    }
    return false;
}

}  // namespace

struct VlcSource::Stream {
    int cat = 0;
    int es_id = -1;
    uint32_t codec = 0;
    CodecId id = CodecId::Unknown;
    std::vector<uint8_t> extra;      // as VLC gave it
    std::vector<uint8_t> ps_annexb;  // video: parameter sets to put in front of keyframes
    unsigned width = 0, height = 0, fps_num = 0, fps_den = 0, rate = 0, channels = 0;
    media::VideoColour colour;
    bool removed = false;
};

#if VPLAYER_HAVE_LIBVLC

VlcSource::VlcSource(libvlc_instance_t *vlc, std::vector<std::string> options)
    : vlc_(vlc), options_(std::move(options)), sink_(std::make_unique<wnk_bridge_sink>()) {
    sink_->abi = WNK_BRIDGE_ABI;
    sink_->opaque = this;
    sink_->add = &VlcSource::on_add;
    sink_->del = &VlcSource::on_del;
    sink_->send = &VlcSource::on_send;
    sink_->flush = &VlcSource::on_flush;
}

VlcSource::~VlcSource() {
    stop_player();
    if (media_ != nullptr) libvlc_media_release(media_);
}

std::string VlcSource::audio_path_text() const {
    switch (audio_path_) {
        case media::AudioPath::Passthrough: return "as it is";
        case media::AudioPath::Ac3: return "re-encoded to AC-3 by VLC";
        case media::AudioPath::Pcm: return "decoded to PCM by VLC";
        default: return "none";
    }
}

bool VlcSource::open(const std::string &uri, std::string *error) {
    uri_ = uri;
    const bool url = uri.find("://") != std::string::npos;
    media_ = url ? libvlc_media_new_location(vlc_, uri.c_str()) : libvlc_media_new_path(vlc_, uri.c_str());
    if (media_ == nullptr) {
        if (error) *error = "libVLC cannot open " + uri;
        return false;
    }
    // Parse: the track list without playing. Interruptible, bounded.
    libvlc_media_parse_with_options(media_, static_cast<libvlc_media_parse_flag_t>(libvlc_media_parse_local | libvlc_media_parse_network), 10000);
    for (int i = 0; i < 1100; ++i) {
        const libvlc_media_parsed_status_t s = libvlc_media_get_parsed_status(media_);
        if (s != 0) break;
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (interrupted_) {
                libvlc_media_parse_stop(media_);
                if (error) *error = "interrupted";
                return false;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    info_ = {};
    info_.uri = uri;
    const size_t dot = uri.rfind('.');
    info_.container = dot == std::string::npos ? "" : uri.substr(dot + 1);
    const libvlc_time_t d = libvlc_media_get_duration(media_);
    info_.duration_us = d > 0 ? d * 1000 : kNoTime;
    info_.seekable = !url || uri.rfind("file://", 0) == 0 || d > 0;

    libvlc_media_track_t **tracks = nullptr;
    const unsigned n = libvlc_media_tracks_get(media_, &tracks);
    bool first_video = true, first_audio = true;
    for (unsigned i = 0; i < n; ++i) {
        const libvlc_media_track_t *t = tracks[i];
        media::TrackInfo ti;
        ti.index = t->i_id;
        ti.codec = codec_of(t->i_codec);
        ti.codec_label = fourcc_text(t->i_codec);
        ti.language = t->psz_language ? t->psz_language : "";
        ti.title = t->psz_description ? t->psz_description : "";
        if (t->i_type == libvlc_track_video && t->video) {
            ti.type = media::TrackType::Video;
            ti.width = static_cast<int>(t->video->i_width);
            ti.height = static_cast<int>(t->video->i_height);
            ti.frame_rate = t->video->i_frame_rate_den ? static_cast<double>(t->video->i_frame_rate_num) / t->video->i_frame_rate_den : 0;
            ti.is_default = first_video;
            first_video = false;
        } else if (t->i_type == libvlc_track_audio && t->audio) {
            ti.type = media::TrackType::Audio;
            ti.sample_rate = static_cast<int>(t->audio->i_rate);
            ti.channels = static_cast<int>(t->audio->i_channels);
            ti.decodable = true;      // VLC decodes every audio codec it knows...
            ti.ac3_encodable = true;  // ...and can re-encode it to AC-3 (#transcode)
            ti.is_default = first_audio;
            first_audio = false;
        } else if (t->i_type == libvlc_track_text) {
            ti.type = media::TrackType::Subtitle;  // not delivered yet (SubtitleKind::None)
        } else {
            continue;
        }
        info_.tracks.push_back(std::move(ti));
    }
    if (n > 0) libvlc_media_tracks_release(tracks, n);
    WNK_LOGI(kCat, "open %s: %zu tracks, %" PRId64 " ms", uri.c_str(), info_.tracks.size(),
             info_.duration_us == kNoTime ? int64_t{-1} : info_.duration_us / 1000);
    if (info_.tracks.empty()) {
        if (error) *error = "libVLC found no playable track";
        return false;
    }
    return true;
}

std::string VlcSource::sout_chain() const {
    const std::string sink = "wnk_bridge{sink=" + std::to_string(reinterpret_cast<uintptr_t>(sink_.get())) + "}";
    if (want_audio_ >= 0 && audio_path_ == media::AudioPath::Ac3) {
        return "#transcode{acodec=a52,ab=640,channels=" + std::to_string(ac3_channels_) + ",samplerate=48000}:" + sink;
    }
    if (want_audio_ >= 0 && audio_path_ == media::AudioPath::Pcm) {
        return "#transcode{acodec=s16l,channels=" + std::to_string(pcm_channels_) +
               ",samplerate=" + std::to_string(pcm_rate_) + "}:" + sink;
    }
    return "#" + sink;
}

bool VlcSource::select(int video_track, int audio_track, const media::BackendCaps &caps, media::StreamFormat *out,
                       std::string *error) {
    stop_player();
    caps_ = caps;
    want_video_ = video_track;
    want_audio_ = audio_track;
    const media::TrackInfo *v = video_track >= 0 ? info_.track(video_track) : nullptr;
    const media::TrackInfo *a = audio_track >= 0 ? info_.track(audio_track) : nullptr;
    if (v != nullptr && !caps.accepts(v->codec)) {
        if (error) *error = "the TV decoder does not take " + (v->codec_label.empty() ? "this video" : v->codec_label);
        return false;
    }
    audio_path_ = media::AudioPath::None;
    if (a != nullptr) {
        audio_path_ = media::audio_path(caps, *a);
        if (audio_path_ == media::AudioPath::None) {
            if (error) *error = "the TV cannot be given audio " + a->codec_label;
            return false;
        }
        pcm_channels_ = caps.pcm_max_channels > 0 ? std::min(std::max(a->channels, 1), caps.pcm_max_channels) : 2;
        pcm_rate_ = media::pcm_output_rate(a->sample_rate > 0 ? a->sample_rate : 48000, caps.pcm_rates);
        ac3_channels_ = std::min(std::max(a->channels, 1), 6);
    }
    WNK_LOGI(kCat, "select video %d audio %d (%s)", video_track, audio_track, media::audio_path_name(audio_path_));
    if (!start_player(error)) return false;

    // Wait for the selected streams to be added (their formats come from VLC's packetizers).
    std::unique_lock<std::mutex> lock(mu_);
    const bool ok = cv_.wait_for(lock, std::chrono::seconds(15), [this] {
        return interrupted_ || ((want_video_ < 0 || video_) && (want_audio_ < 0 || audio_));
    });
    if (interrupted_) {
        if (error) *error = "interrupted";
        return false;
    }
    if (!ok) {
        if (error) *error = "libVLC did not deliver the selected streams";
        return false;
    }
    *out = {};
    if (video_ != nullptr) {
        media::VideoFormat vf;
        vf.codec = video_->id;
        vf.width = static_cast<int>(video_->width);
        vf.height = static_cast<int>(video_->height);
        vf.frame_rate = video_->fps_den ? static_cast<double>(video_->fps_num) / video_->fps_den : 0;
        if (vf.frame_rate <= 0 && v != nullptr) vf.frame_rate = v->frame_rate;
        vf.colour = video_->colour;
        out->video = vf;
    }
    if (audio_ != nullptr) {
        media::AudioFormat af;
        af.codec = audio_->id;
        af.sample_rate = static_cast<int>(audio_->rate);
        af.channels = static_cast<int>(audio_->channels);
        if (audio_->id == CodecId::Opus) af.codec_header = audio_->extra;
        out->audio = af;
    }
    return true;
}

bool VlcSource::start_player(std::string *error) {
    libvlc_media_t *m = libvlc_media_duplicate(media_);
    const std::string chain = ":sout=" + sout_chain();
    libvlc_media_add_option(m, chain.c_str());
    libvlc_media_add_option(m, ":no-sout-spu");
    libvlc_media_add_option(m, ":input-fast-seek");  // seek to keyframes; the decoder starts there anyway
    if (want_video_ >= 0) {
        libvlc_media_add_option(m, (":video-track-id=" + std::to_string(want_video_)).c_str());
    } else {
        libvlc_media_add_option(m, ":no-sout-video");
    }
    if (want_audio_ >= 0) {
        libvlc_media_add_option(m, (":audio-track-id=" + std::to_string(want_audio_)).c_str());
    } else {
        libvlc_media_add_option(m, ":no-sout-audio");
    }
    for (const auto &o : options_) libvlc_media_add_option(m, o.c_str());
    {
        std::lock_guard<std::mutex> lock(mu_);
        stopping_ = false;
        queue_.clear();
        queued_bytes_ = 0;
        streams_.clear();
        video_ = audio_ = nullptr;
    }
    mp_ = libvlc_media_player_new_from_media(m);
    libvlc_media_release(m);
    if (mp_ == nullptr || libvlc_media_player_play(mp_) != 0) {
        if (error) *error = "libVLC could not start";
        return false;
    }
    WNK_LOGI(kCat, "playing with %s", chain.c_str());
    return true;
}

void VlcSource::stop_player() {
    if (mp_ == nullptr) return;
    {
        std::lock_guard<std::mutex> lock(mu_);
        stopping_ = true;  // blocked sends return, so stop() can finish
    }
    space_.notify_all();
    libvlc_media_player_stop(mp_);
    libvlc_media_player_release(mp_);
    mp_ = nullptr;
}

// ---- bridge callbacks (VLC threads) ----

void *VlcSource::on_add(void *opaque, const wnk_es_desc *d) {
    auto *self = static_cast<VlcSource *>(opaque);
    std::lock_guard<std::mutex> lock(self->mu_);
    if (d->cat != WNK_ES_VIDEO && d->cat != WNK_ES_AUDIO) return nullptr;
    if (d->cat == WNK_ES_VIDEO && (self->want_video_ < 0 || self->video_ != nullptr)) return nullptr;
    if (d->cat == WNK_ES_AUDIO && (self->want_audio_ < 0 || self->audio_ != nullptr)) return nullptr;
    auto s = std::make_unique<Stream>();
    s->cat = d->cat;
    s->es_id = d->es_id;
    s->codec = d->codec;
    s->id = codec_of(d->codec);
    if (d->extra && d->extra_size) s->extra.assign(d->extra, d->extra + d->extra_size);
    s->width = d->visible_width ? d->visible_width : d->width;
    s->height = d->visible_height ? d->visible_height : d->height;
    s->fps_num = d->fps_num;
    s->fps_den = d->fps_den;
    s->rate = d->rate;
    s->channels = d->channels;
    if (d->cat == WNK_ES_VIDEO) {
        s->colour.primaries = d->primaries;
        s->colour.transfer = d->transfer;
        s->colour.matrix = d->matrix;
        s->colour.full_range = d->full_range != 0;
        if (d->has_mastering) {
            s->colour.has_primaries = true;
            std::memcpy(s->colour.display_primaries, d->display_primaries, sizeof d->display_primaries);
            s->colour.white_point[0] = d->white_point[0];
            s->colour.white_point[1] = d->white_point[1];
            s->colour.has_luminance = true;
            s->colour.max_luminance = d->max_luminance;
            s->colour.min_luminance = d->min_luminance;
        }
        if (d->has_light_level) {
            s->colour.has_light_level = true;
            s->colour.max_cll = d->max_cll;
            s->colour.max_fall = d->max_fall;
        }
        const bool hevc = s->id == CodecId::Hevc;
        if (s->id == CodecId::H264 || hevc)
            s->ps_annexb = is_annexb(s->extra) ? s->extra : config_to_annexb(s->extra, hevc);
    }
    WNK_LOGI(kCat, "%s stream %d: %s %ux%u %uch %uHz, %zu bytes of setup", d->cat == WNK_ES_VIDEO ? "video" : "audio",
             d->es_id, fourcc_text(d->codec).c_str(), s->width, s->height, s->channels, s->rate, s->extra.size());
    Stream *raw = s.get();
    (d->cat == WNK_ES_VIDEO ? self->video_ : self->audio_) = raw;
    self->streams_.push_back(std::move(s));
    self->cv_.notify_all();
    return raw;
}

void VlcSource::on_del(void *opaque, void *id) {
    auto *self = static_cast<VlcSource *>(opaque);
    std::lock_guard<std::mutex> lock(self->mu_);
    auto *s = static_cast<Stream *>(id);
    s->removed = true;
    if (self->video_ == s) self->video_ = nullptr;
    if (self->audio_ == s) self->audio_ = nullptr;
    self->queue_.erase(std::remove_if(self->queue_.begin(), self->queue_.end(), [s](const Item &i) { return i.stream == s; }),
                       self->queue_.end());
    self->queued_bytes_ = 0;
    for (const auto &i : self->queue_) self->queued_bytes_ += i.data.size();
}

int VlcSource::on_send(void *opaque, void *id, const uint8_t *data, size_t size, int64_t pts, int64_t dts,
                       int64_t length, unsigned flags) {
    auto *self = static_cast<VlcSource *>(opaque);
    auto *s = static_cast<Stream *>(id);
    std::unique_lock<std::mutex> lock(self->mu_);
    if (self->stopping_ || s->removed) return 0;

    // Rebase streams that start far from zero (MPEG-TS clocks), so content time matches libVLC's.
    const int64_t t0 = pts != WNK_BRIDGE_NO_TIME ? pts : dts;
    if (!self->base_known_ && t0 != WNK_BRIDGE_NO_TIME) {
        self->base_known_ = true;
        self->base_us_ = t0 > kRebaseAboveUs ? t0 : 0;
    }
    if (pts != WNK_BRIDGE_NO_TIME) pts -= self->base_us_;
    if (dts != WNK_BRIDGE_NO_TIME) dts -= self->base_us_;
    const int64_t when = pts != WNK_BRIDGE_NO_TIME ? pts : dts;
    const bool key = (flags & WNK_BLOCK_KEY) != 0;

    // After a seek: wait for a video keyframe near the target; audio from just before it on.
    if (self->seeking_) {
        const bool near = when != WNK_BRIDGE_NO_TIME && std::llabs(when - self->seek_target_us_) < kSeekWindowUs;
        const bool video = self->video_ != nullptr;
        if (!near || (video && (s != self->video_ || !key)) || (!video && s != self->audio_)) return 0;
        self->seeking_ = false;
        self->audio_from_us_ = video ? when - 100'000 : kNoTime;
        WNK_LOGI(kCat, "seek landed at %" PRId64 " ms (asked %" PRId64 ")", when / 1000, self->seek_target_us_ / 1000);
    }
    if (s == self->audio_ && self->audio_from_us_ != kNoTime) {
        if (when != WNK_BRIDGE_NO_TIME && when < self->audio_from_us_) return 0;
        self->audio_from_us_ = kNoTime;
    }

    // Back-pressure: wait for room (the controller reads as it feeds the decoder).
    self->space_.wait(lock, [self, s] {
        return self->stopping_ || s->removed || self->seeking_ ||
               (self->queued_bytes_ < kMaxQueueBytes && self->queue_.size() < kMaxQueueItems);
    });
    if (self->stopping_ || s->removed || self->seeking_) return 0;

    Item item;
    item.stream = s;
    item.data.assign(data, data + size);
    item.pts = pts == WNK_BRIDGE_NO_TIME ? kNoTime : pts;
    item.dts = dts == WNK_BRIDGE_NO_TIME ? kNoTime : dts;
    item.duration = length > 0 ? length : 0;
    item.key = key;
    self->queued_bytes_ += item.data.size();
    self->queue_.push_back(std::move(item));
    self->cv_.notify_all();
    return 0;
}

void VlcSource::on_flush(void *opaque, void *id) {
    auto *self = static_cast<VlcSource *>(opaque);
    std::lock_guard<std::mutex> lock(self->mu_);
    auto *s = static_cast<Stream *>(id);
    self->queue_.erase(std::remove_if(self->queue_.begin(), self->queue_.end(), [s](const Item &i) { return i.stream == s; }),
                       self->queue_.end());
    self->queued_bytes_ = 0;
    for (const auto &i : self->queue_) self->queued_bytes_ += i.data.size();
    self->space_.notify_all();
}

// ---- reading (the controller's reader thread) ----

void VlcSource::to_packet(Item &item, media::Packet *out) {
    Stream *s = item.stream;
    *out = {};
    out->type = s->cat == WNK_ES_VIDEO ? media::TrackType::Video : media::TrackType::Audio;
    out->pts_us = item.pts;
    out->dts_us = item.dts;
    out->duration_us = item.duration;
    out->key = item.key;
    if (s->id == CodecId::H264 || s->id == CodecId::Hevc) {
        const bool hevc = s->id == CodecId::Hevc;
        if (item.key && !s->ps_annexb.empty() && is_annexb(item.data) && !has_sps(item.data, hevc)) {
            out->data.reserve(s->ps_annexb.size() + item.data.size());
            out->data = s->ps_annexb;
            out->data.insert(out->data.end(), item.data.begin(), item.data.end());
        } else {
            out->data = std::move(item.data);
        }
        if (hevc) media::hevc_strip_hdr10plus(&out->data);
    } else if (s->id == CodecId::Av1) {
        const bool has_td = item.data.size() >= 2 && (item.data[0] >> 3 & 0x0F) == 2;  // OBU_TEMPORAL_DELIMITER
        out->data = has_td ? std::move(item.data)
                           : media::av1_temporal_unit(item.data.data(), item.data.size(), s->extra, item.key);
    } else if (s->id == CodecId::Aac && caps_.aac_adts && audio_path_ == media::AudioPath::Passthrough) {
        const bool adts = item.data.size() >= 2 && item.data[0] == 0xFF && (item.data[1] & 0xF0) == 0xF0;
        uint8_t h[7];
        if (!adts && media::adts_header(s->extra, item.data.size(), h)) {
            out->data.reserve(7 + item.data.size());
            out->data.assign(h, h + 7);
            out->data.insert(out->data.end(), item.data.begin(), item.data.end());
        } else {
            out->data = std::move(item.data);
        }
    } else {
        out->data = std::move(item.data);
    }
}

media::MediaSource::Read VlcSource::read(media::Packet *out, std::string *error) {
    std::unique_lock<std::mutex> lock(mu_);
    for (;;) {
        if (interrupted_) return Read::Interrupted;
        if (!queue_.empty()) {
            Item item = std::move(queue_.front());
            queue_.pop_front();
            queued_bytes_ -= std::min(queued_bytes_, item.data.size());
            space_.notify_all();
            lock.unlock();
            to_packet(item, out);
            return Read::Packet;
        }
        const libvlc_state_t st = mp_ ? libvlc_media_player_get_state(mp_) : libvlc_Error;
        if (st == libvlc_Playing || st == libvlc_Paused) started_ = true;
        if (st == libvlc_Error) {
            if (error) *error = "libVLC stopped with an error";
            return Read::Error;
        }
        if (st == libvlc_Ended || (started_ && st == libvlc_Stopped)) return Read::EndOfStream;
        cv_.wait_for(lock, std::chrono::milliseconds(100));
    }
}

bool VlcSource::seek(int64_t pts_us, std::string *error) {
    if (mp_ == nullptr) {
        if (error) *error = "not playing";
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(mu_);
        queue_.clear();
        queued_bytes_ = 0;
        seeking_ = true;
        seek_target_us_ = std::max<int64_t>(0, pts_us);
        audio_from_us_ = kNoTime;
    }
    space_.notify_all();
    // A finished input cannot seek: start it again first.
    const libvlc_state_t st = libvlc_media_player_get_state(mp_);
    if (st == libvlc_Ended || st == libvlc_Stopped || st == libvlc_Error) {
        libvlc_media_player_stop(mp_);
        {
            std::lock_guard<std::mutex> lock(mu_);
            stopping_ = false;
            streams_.clear();
            video_ = audio_ = nullptr;
        }
        libvlc_media_player_play(mp_);
    }
    libvlc_media_player_set_time(mp_, std::max<int64_t>(0, pts_us) / 1000);
    WNK_LOGI(kCat, "seek to %" PRId64 " ms", pts_us / 1000);
    return true;
}

void VlcSource::interrupt() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        interrupted_ = true;
    }
    cv_.notify_all();
    space_.notify_all();
}

void VlcSource::reset_interrupt() {
    std::lock_guard<std::mutex> lock(mu_);
    interrupted_ = false;
}

#else  // no libVLC in this build

VlcSource::VlcSource(libvlc_instance_t *vlc, std::vector<std::string> options)
    : vlc_(vlc), options_(std::move(options)) {}
VlcSource::~VlcSource() = default;
std::string VlcSource::audio_path_text() const { return "none"; }
bool VlcSource::open(const std::string &, std::string *error) {
    if (error) *error = "this build has no libVLC";
    return false;
}
bool VlcSource::select(int, int, const media::BackendCaps &, media::StreamFormat *, std::string *error) {
    if (error) *error = "this build has no libVLC";
    return false;
}
media::MediaSource::Read VlcSource::read(media::Packet *, std::string *) { return Read::Error; }
bool VlcSource::seek(int64_t, std::string *) { return false; }
void VlcSource::interrupt() {}
void VlcSource::reset_interrupt() {}
void *VlcSource::on_add(void *, const wnk_es_desc *) { return nullptr; }
void VlcSource::on_del(void *, void *) {}
int VlcSource::on_send(void *, void *, const uint8_t *, size_t, int64_t, int64_t, int64_t, unsigned) { return 0; }
void VlcSource::on_flush(void *, void *) {}
bool VlcSource::start_player(std::string *) { return false; }
void VlcSource::stop_player() {}
std::string VlcSource::sout_chain() const { return {}; }
void VlcSource::to_packet(Item &, media::Packet *) {}

#endif

}  // namespace vplayer
