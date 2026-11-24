// SPDX-License-Identifier: GPL-3.0-or-later
#include "player/vlc_source.hpp"

#include "player/dovi_detect.hpp"
#include "player/mkv_attachments.hpp"
#include "player/subtitle_bitmap.hpp"
#include "player/teletext.hpp"
#include "player/subtitle_text.hpp"
#include "vlc_engine.hpp"

#include "wnk_bridge/wnk_bridge.h"

#include "wnk/log.hpp"
#include "wnk/media/bitstream.hpp"
#include "wnk/media/dovi.hpp"
#include "wnk/media/time_stretch.hpp"

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
constexpr auto kFlushWait = std::chrono::seconds(3);  // how long a seek waits for VLC's flush
constexpr int64_t kSeekToleranceUs = 20'000;          // a keyframe this close to the target is at it
constexpr size_t kMaxSeekGopBytes = 32u << 20;        // a longer GOP is handed over as it is
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
    std::string encoding;  // subtitles: the declared character set, if any
    std::unique_ptr<BitmapSubtitleDecoder> bitmap;  // PGS, DVD, DVB subtitles
    std::unique_ptr<TeletextDecoder> teletext;      // teletext subtitles
    bool removed = false;
    bool awaiting_flush = false;  // a seek was asked: what arrives before VLC's flush is from before it
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

std::string VlcSource::dovi_text() const {
    std::lock_guard<std::mutex> lock(mu_);
    return dovi_text_;
}

std::vector<media::FontAttachment> VlcSource::fonts() const {
    std::lock_guard<std::mutex> lock(mu_);
    return fonts_;
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

    // Dolby Vision: VLC 3 does not report it; the configuration record is read from local files.
    dovi_record_ = {};
    if (!url && find_dovi_config(uri, &dovi_record_))
        WNK_LOGI(kCat, "Dolby Vision record: profile %d, level %d, rpu %d el %d bl %d, compatibility %d",
                 dovi_record_.profile, dovi_record_.level, dovi_record_.rpu, dovi_record_.el, dovi_record_.bl,
                 dovi_record_.bl_compatibility);

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
            ti.type = media::TrackType::Subtitle;
            ti.subtitle = subtitle_kind(t->i_codec);  // text, PGS, DVD, DVB; None for teletext, captions
        } else {
            continue;
        }
        info_.tracks.push_back(std::move(ti));
    }
    if (n > 0) libvlc_media_tracks_release(tracks, n);
    // Styled subtitles' fonts: libVLC does not hand out attachments, so they are read from the file.
    const bool ass = std::any_of(info_.tracks.begin(), info_.tracks.end(), [](const media::TrackInfo &t) {
        return t.type == media::TrackType::Subtitle && t.codec_label == "ssa";
    });
    if (ass && !url) {
        auto fonts = mkv_fonts(uri);
        if (!fonts.empty()) WNK_LOGI(kCat, "%zu fonts attached for styled subtitles", fonts.size());
        std::lock_guard<std::mutex> lock(mu_);
        fonts_ = std::move(fonts);
    }
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
        const double speed = speed_.load();
        if (force_pcm_.load() && audio_path_ != media::AudioPath::Pcm) {
            if (!caps.accepts(CodecId::PcmS16)) {
                if (error) *error = "the TV decoder takes no PCM";
                return false;
            }
            audio_path_ = media::AudioPath::Pcm;  // Settings: always decoded
        }
        if (speed != 1.0) {
            // Another speed: VLC decodes to stereo PCM, which is time-stretched here (pitch kept).
            if (!caps.accepts(CodecId::PcmS16)) {
                if (error) *error = "the TV decoder takes no PCM, which other speeds need";
                return false;
            }
            audio_path_ = media::AudioPath::Pcm;
        }
        if (audio_path_ == media::AudioPath::None) {
            if (error) *error = "the TV cannot be given audio " + a->codec_label;
            return false;
        }
        pcm_channels_ = caps.pcm_max_channels > 0 ? std::min(std::max(a->channels, 1), caps.pcm_max_channels) : 2;
        if (speed != 1.0) pcm_channels_ = 2;
        pcm_rate_ = media::pcm_output_rate(a->sample_rate > 0 ? a->sample_rate : 48000, caps.pcm_rates);
        ac3_channels_ = std::min(std::max(a->channels, 1), 6);
    }
    stretch_.reset();
    if (a != nullptr && speed_.load() != 1.0)
        stretch_ = std::make_unique<media::TimeStretch>(2, pcm_rate_, speed_.load());
    stretch_base_us_ = stretch_in_us_ = kNoTime;
    stretch_frames_ = 0;
    WNK_LOGI(kCat, "select video %d audio %d (%s)%s", video_track, audio_track, media::audio_path_name(audio_path_),
             stretch_ ? (", " + std::to_string(speed_.load()).substr(0, 4) + "x stretched").c_str() : "");
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
        if (video_->id == CodecId::Hevc) {
            // Without a record, the first keyframe tells: RPUs, an enhancement layer.
            DoviNals nals;
            if (dovi_record_.profile < 0) {
                auto first_video = [this] {
                    return std::find_if(queue_.begin(), queue_.end(), [this](const Item &i) {
                        return i.stream == video_ && i.key;
                    });
                };
                cv_.wait_for(lock, std::chrono::seconds(2), [&] { return interrupted_ || first_video() != queue_.end(); });
                const auto it = first_video();
                if (it != queue_.end()) nals = scan_dovi_nals(it->data.data(), it->data.size());
            }
            const DoviPlan plan = dovi_plan(dovi_record_, nals, video_->colour, media::dovi_available());
            vf.dolby_vision_profile = plan.profile;
            dovi_convert_ = plan.convert_p7;
            dovi_text_ = plan.how;
            dovi_stats_ = {};
            if (!plan.how.empty()) WNK_LOGI(kCat, "Dolby Vision: %s", plan.how.c_str());
        }
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
    {
        // The viewer's subtitle choice and files survive a restart (another audio track, a seek
        // after the end). A file's stream id may differ in the new player; libVLC selects the file.
        std::lock_guard<std::mutex> lock(player_mu_);
        for (const auto &f : subtitle_files_)
            libvlc_media_slaves_add(m, libvlc_media_slave_type_subtitle, 4, file_uri(f).c_str());
        if (subtitle_choice_ >= 0) {
            libvlc_media_add_option(m, (":sub-track-id=" + std::to_string(subtitle_choice_)).c_str());
        } else if (subtitle_choice_ == -1) {
            libvlc_media_add_option(m, ":sub-language=none");
        }
    }
    // The media's own timestamps: VLC 3 converts a sout's to its playback clock, which restarts
    // at every seek (libvlc/patches/vlc/0004 adds the option).
    libvlc_media_add_option(m, ":sout-stream-time");
    // Only the selected streams (VLC 3 streams every one by default, transcoding each audio track).
    libvlc_media_add_option(m, ":no-sout-all");
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
    libvlc_media_player_t *mp = libvlc_media_player_new_from_media(m);
    libvlc_media_release(m);
    {
        std::lock_guard<std::mutex> lock(player_mu_);
        mp_ = mp;
    }
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
    libvlc_media_player_t *old;
    {
        std::lock_guard<std::mutex> lock(player_mu_);
        old = mp_;
        mp_ = nullptr;
    }
    libvlc_media_player_release(old);  // the main thread may still hold a reference
}

// ---- bridge callbacks (VLC threads) ----

void *VlcSource::on_add(void *opaque, const wnk_es_desc *d) {
    auto *self = static_cast<VlcSource *>(opaque);
    std::lock_guard<std::mutex> lock(self->mu_);
    if (d->cat == WNK_ES_SUBTITLE) {
        auto &seen = self->seen_subtitles_[d->es_id];
        seen.codec = d->codec;
        const media::SubtitleKind kind = subtitle_kind(d->codec);
        if (kind == media::SubtitleKind::None) {
            WNK_LOGI(kCat, "subtitle stream %d: %s, not drawn on this path yet", d->es_id, fourcc_text(d->codec).c_str());
            return nullptr;
        }
        auto s = std::make_unique<Stream>();
        s->cat = d->cat;
        s->es_id = d->es_id;
        s->codec = d->codec;
        if (d->encoding) s->encoding = d->encoding;
        if (d->codec == fourcc('t', 'e', 'l', 'x'))
            s->teletext = std::make_unique<TeletextDecoder>(d->teletext_magazine, d->teletext_page);
        if (kind == media::SubtitleKind::Bitmap)
            s->bitmap = std::make_unique<BitmapSubtitleDecoder>(d->codec, d->has_palette ? d->palette : nullptr,
                                                                static_cast<int>(d->canvas_width),
                                                                static_cast<int>(d->canvas_height));
        if (d->codec == fourcc('s', 's', 'a', ' ') && d->extra && d->extra_size) {
            size_t n = d->extra_size;
            while (n > 0 && d->extra[n - 1] == 0) --n;
            seen.ass_header.assign(reinterpret_cast<const char *>(d->extra), n);
        }
        WNK_LOGI(kCat, "subtitle stream %d: %s%s%s", d->es_id, fourcc_text(d->codec).c_str(),
                 d->encoding ? ", " : "", d->encoding ? d->encoding : "");
        Stream *raw = s.get();
        self->streams_.push_back(std::move(s));
        return raw;
    }
    if (d->cat != WNK_ES_VIDEO && d->cat != WNK_ES_AUDIO) return nullptr;
    // Only the tracks select() asked for (VLC may offer others: a track changing, sout-all).
    if (d->cat == WNK_ES_VIDEO && (d->es_id != self->want_video_ || self->video_ != nullptr)) return nullptr;
    if (d->cat == WNK_ES_AUDIO && (d->es_id != self->want_audio_ || self->audio_ != nullptr)) return nullptr;
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

    // Subtitles: a cue now, queued without waiting for room and never dropped by a seek (the
    // controller's store recognises a cue it already has).
    if (s->cat == WNK_ES_SUBTITLE) {
        const int64_t t = pts != WNK_BRIDGE_NO_TIME ? pts : dts;
        if (t == WNK_BRIDGE_NO_TIME) return 0;
        std::vector<std::shared_ptr<const media::SubtitleCue>> cues;
        if (s->teletext) {
            cues = s->teletext->decode(data, size, s->es_id, t - self->base_us_);
        } else if (s->bitmap) {
            if (self->video_ != nullptr)
                s->bitmap->set_video_size(static_cast<int>(self->video_->width), static_cast<int>(self->video_->height));
            cues = s->bitmap->decode(data, size, s->es_id, t - self->base_us_);
        } else if (auto cue = text_subtitle_cue(s->codec, data, size, s->encoding.empty() ? nullptr : s->encoding.c_str(),
                                                s->es_id, t - self->base_us_, length, self->subrip_all_ass_.load())) {
            cues.push_back(std::move(cue));
        }
        for (auto &cue : cues) {
            Item item;
            item.stream = s;
            item.pts = cue->start_us;
            item.cue = std::move(cue);
            self->queue_.push_back(std::move(item));
        }
        if (!cues.empty()) self->cv_.notify_all();
        return 0;
    }

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

    Item item;
    item.stream = s;
    item.pts = pts == WNK_BRIDGE_NO_TIME ? kNoTime : pts;
    item.dts = dts == WNK_BRIDGE_NO_TIME ? kNoTime : dts;
    item.duration = length > 0 ? length : 0;
    item.key = key;

    // After a seek. Packets read before it are still on their way: VLC flushes each stream when its
    // input seeks (through #transcode too, with our patch), on the stream's own thread after the old
    // packets, and everything before that flush is dropped (should none come, after kFlushWait).
    // Then VLC's fast seek starts reading from an index point that can be well before the target
    // (Matroska: 9 s for a 10 s skip), so the newest video keyframe at or before the target is kept
    // with everything after it, and handed over once a packet reaches the target: playback restarts
    // at most one GOP before it. Without video, audio from the target on.
    if (self->seeking_) {
        if (s->awaiting_flush && std::chrono::steady_clock::now() - self->seek_started_ < kFlushWait) return 0;
        if (s != self->video_ && s != self->audio_) return 0;
        const int64_t target = self->seek_target_us_;
        if (self->video_ == nullptr) {
            if (when == WNK_BRIDGE_NO_TIME || when < target - 100'000) return 0;
            self->land_seek_locked(when);
        } else if (s == self->video_ && key && when != WNK_BRIDGE_NO_TIME && when <= target + kSeekToleranceUs) {
            self->seek_gop_.clear();  // a newer keyframe before the target: restart there
            self->seek_gop_bytes_ = 0;
            self->seek_gop_key_us_ = when;
            item.data.assign(data, data + size);
            self->seek_gop_bytes_ += item.data.size();
            self->seek_gop_.push_back(std::move(item));
            if (when >= target - kSeekToleranceUs) self->commit_seek_gop_locked();
            return 0;
        } else if (self->seek_gop_.empty()) {
            // No keyframe before the target seen: VLC started past it. Land on the first keyframe.
            if (s != self->video_ || !key) return 0;
            self->land_seek_locked(when);
        } else {
            item.data.assign(data, data + size);
            self->seek_gop_bytes_ += item.data.size();
            self->seek_gop_.push_back(std::move(item));
            const bool reached = s == self->video_ && when != WNK_BRIDGE_NO_TIME && when >= target;
            if (reached || self->seek_gop_bytes_ > kMaxSeekGopBytes) self->commit_seek_gop_locked();
            return 0;
        }
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

    item.data.assign(data, data + size);
    self->queued_bytes_ += item.data.size();
    self->queue_.push_back(std::move(item));
    self->cv_.notify_all();
    return 0;
}

// mu_ held. The seek ends at `at` (a video keyframe, or audio without video).
void VlcSource::land_seek_locked(int64_t at) {
    seeking_ = false;
    audio_from_us_ = video_ != nullptr ? at - 100'000 : kNoTime;
    seek_gop_.clear();
    seek_gop_bytes_ = 0;
    WNK_LOGI(kCat, "seek landed at %" PRId64 " ms (asked %" PRId64 ")", at / 1000, seek_target_us_ / 1000);
}

// mu_ held. Hands the buffered GOP (keyframe at or before the target, and what followed) to the
// reader; audio from just before the keyframe on.
void VlcSource::commit_seek_gop_locked() {
    std::deque<Item> gop;
    gop.swap(seek_gop_);
    land_seek_locked(seek_gop_key_us_);
    for (auto &i : gop) {
        if (i.stream == audio_ && i.pts != kNoTime && i.pts < audio_from_us_) continue;
        queued_bytes_ += i.data.size();
        queue_.push_back(std::move(i));
    }
    cv_.notify_all();
}

void VlcSource::on_flush(void *opaque, void *id) {
    auto *self = static_cast<VlcSource *>(opaque);
    std::lock_guard<std::mutex> lock(self->mu_);
    auto *s = static_cast<Stream *>(id);
    s->awaiting_flush = false;
    self->queue_.erase(std::remove_if(self->queue_.begin(), self->queue_.end(), [s](const Item &i) { return i.stream == s; }),
                       self->queue_.end());
    self->queued_bytes_ = 0;
    for (const auto &i : self->queue_) self->queued_bytes_ += i.data.size();
    self->space_.notify_all();
}

// ---- reading (the controller's reader thread) ----

bool VlcSource::set_speed(double speed, std::string *why) {
    if (!(speed >= 0.25 && speed <= 4.0)) {
        if (why != nullptr) *why = "speed out of range";
        return false;
    }
    speed_.store(speed);
    return true;
}

// Stereo S16 PCM through the stretcher. Output is stamped from the first frame after a gap by the
// frames it gave, times the speed: content time, which the backend divides again (as the kit's
// FFmpeg source does). False while the stretcher is still filling its first window.
bool VlcSource::stretch_audio(media::Packet *p) {
    const int frames = static_cast<int>(p->data.size() / 4);
    if (frames == 0) return false;
    const int rate = std::max(1, pcm_rate_);
    const int64_t dur = static_cast<int64_t>(frames) * 1000000 / rate;
    if (p->pts_us != kNoTime && (stretch_in_us_ == kNoTime || std::llabs(p->pts_us - stretch_in_us_) > 100000)) {
        stretch_->reset();
        stretch_base_us_ = p->pts_us;
        stretch_frames_ = 0;
    }
    if (p->pts_us != kNoTime) stretch_in_us_ = p->pts_us + dur;
    if (stretch_base_us_ == kNoTime) return false;
    const auto *s16 = reinterpret_cast<const int16_t *>(p->data.data());
    stretch_in_.resize(static_cast<size_t>(frames) * 2);
    for (size_t k = 0; k < stretch_in_.size(); ++k) stretch_in_[k] = s16[k] / 32768.0f;
    stretch_out_.clear();
    stretch_->process(stretch_in_.data(), frames, &stretch_out_);
    const int got = static_cast<int>(stretch_out_.size() / 2);
    if (got == 0) return false;
    p->data.resize(static_cast<size_t>(got) * 4);
    auto *o = reinterpret_cast<int16_t *>(p->data.data());
    for (size_t k = 0; k < stretch_out_.size(); ++k)
        o[k] = static_cast<int16_t>(std::lrint(std::clamp(stretch_out_[k], -1.0f, 1.0f) * 32767.0f));
    const double speed = stretch_->speed();
    p->pts_us = p->dts_us = stretch_base_us_ + static_cast<int64_t>(std::llround(stretch_frames_ * speed * 1e6 / rate));
    p->duration_us = static_cast<int64_t>(std::llround(got * speed * 1e6 / rate));
    stretch_frames_ += got;
    return true;
}

bool VlcSource::to_packet(Item &item, media::Packet *out) {
    Stream *s = item.stream;
    *out = {};
    if (item.cue) {
        out->type = media::TrackType::Subtitle;
        out->pts_us = item.pts;
        out->subtitle = std::move(item.cue);
        return true;
    }
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
        if (hevc && dovi_convert_) {
            const uint64_t dropped = dovi_stats_.rpu_dropped;
            media::hevc_dovi_p7_to_p81(&out->data,
                                       media::dovi_available() ? media::RpuRewrite(&media::dovi_rpu_to_p81)
                                                               : media::RpuRewrite(),
                                       &dovi_stats_);
            const uint64_t n = dovi_stats_.rpu_converted + dovi_stats_.rpu_dropped;
            if (n == 1 || n == 1000)
                WNK_LOGI(kCat, "Dolby Vision Profile 7: %llu RPUs converted, %llu dropped, %llu enhancement-layer units removed",
                         static_cast<unsigned long long>(dovi_stats_.rpu_converted),
                         static_cast<unsigned long long>(dovi_stats_.rpu_dropped),
                         static_cast<unsigned long long>(dovi_stats_.el_dropped));
            if (dovi_stats_.rpu_dropped > dropped && dropped == 0)
                WNK_LOGW(kCat, "Dolby Vision RPU dropped: %s", dovi_stats_.last_error.c_str());
        }
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
    if (stretch_ && s->cat == WNK_ES_AUDIO && !stretch_audio(out)) return false;
    if (s->cat == WNK_ES_AUDIO) {  // the viewer's audio delay
        const int64_t d = audio_delay_us_.load();
        if (d != 0) {
            if (out->pts_us != kNoTime) out->pts_us += d;
            if (out->dts_us != kNoTime) out->dts_us += d;
        }
    }
    return true;
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
            if (to_packet(item, out)) return Read::Packet;
            lock.lock();
            continue;
        }
        const libvlc_state_t st = mp_ ? libvlc_media_player_get_state(mp_) : libvlc_Error;
        if (st == libvlc_Playing || st == libvlc_Paused) started_ = true;
        if (st == libvlc_Error) {
            if (error) *error = "libVLC stopped with an error";
            return Read::Error;
        }
        if (st == libvlc_Ended && seeking_ && !seek_gop_.empty()) {
            commit_seek_gop_locked();  // the input ended before reaching the target: play what is left
            continue;
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
        seek_started_ = std::chrono::steady_clock::now();
        seek_gop_.clear();
        seek_gop_bytes_ = 0;
        audio_from_us_ = kNoTime;
        for (auto &st : streams_)
            if (st->cat != WNK_ES_SUBTITLE) st->awaiting_flush = true;
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

// ---- subtitles (main thread) ----

libvlc_media_player_t *VlcSource::hold_player() const {
    std::lock_guard<std::mutex> lock(player_mu_);
    if (mp_ != nullptr) libvlc_media_player_retain(mp_);
    return mp_;
}

std::vector<VlcSource::SubtitleTrack> VlcSource::subtitle_tracks() const {
    std::vector<SubtitleTrack> out;
    libvlc_media_player_t *mp = hold_player();
    if (mp == nullptr) return out;
    libvlc_track_description_t *list = libvlc_video_get_spu_description(mp);
    libvlc_media_player_release(mp);
    std::lock_guard<std::mutex> lock(mu_);
    for (auto *t = list; t != nullptr; t = t->p_next) {
        SubtitleTrack st;
        st.id = t->i_id;
        st.name = t->psz_name ? t->psz_name : "Track " + std::to_string(t->i_id);
        if (st.id >= 0) {
            const media::TrackInfo *ti = info_.track(st.id);
            const auto seen = seen_subtitles_.find(st.id);
            if (seen != seen_subtitles_.end()) {
                st.kind = subtitle_kind(seen->second.codec);
            } else if (ti != nullptr) {
                st.kind = ti->subtitle;
            } else {
                st.kind = media::SubtitleKind::Text;  // a file libVLC found beside the media: text, mostly
            }
        }
        out.push_back(std::move(st));
    }
    if (list != nullptr) libvlc_track_description_list_release(list);
    if (out.empty()) out.push_back({-1, "Disable", media::SubtitleKind::None});
    return out;
}

int VlcSource::subtitle_track() const {
    libvlc_media_player_t *mp = hold_player();
    if (mp == nullptr) return last_subtitle_.load();
    // libVLC reads ahead of the picture and reports no track once its input has ended; the TV is
    // still playing what was read, so the last answer from a live input stands.
    const libvlc_state_t st = libvlc_media_player_get_state(mp);
    if (st == libvlc_Playing || st == libvlc_Paused || st == libvlc_Buffering)
        last_subtitle_.store(libvlc_video_get_spu(mp));
    libvlc_media_player_release(mp);
    return last_subtitle_.load();
}

void VlcSource::set_subtitle_track(int id) {
    {
        std::lock_guard<std::mutex> lock(player_mu_);
        subtitle_choice_ = id < 0 ? -1 : id;
    }
    last_subtitle_.store(id < 0 ? -1 : id);
    libvlc_media_player_t *mp = hold_player();
    if (mp == nullptr) return;
    libvlc_video_set_spu(mp, id);
    libvlc_media_player_release(mp);
}

bool VlcSource::add_subtitle_file(const std::string &path) {
    {
        std::lock_guard<std::mutex> lock(player_mu_);
        subtitle_files_.push_back(path);
        subtitle_choice_ = -2;  // the file's stream, once libVLC selects it
    }
    libvlc_media_player_t *mp = hold_player();
    if (mp == nullptr) return false;
    const bool ok = libvlc_media_player_add_slave(mp, libvlc_media_slave_type_subtitle, file_uri(path).c_str(), true) == 0;
    libvlc_media_player_release(mp);
    return ok;
}

std::vector<VlcSource::ChapterMark> VlcSource::chapters() const {
    {
        std::lock_guard<std::mutex> lock(player_mu_);
        if (!chapters_.empty()) return chapters_;
    }
    libvlc_media_player_t *mp = hold_player();
    if (mp == nullptr) return {};
    std::vector<ChapterMark> out;
    libvlc_chapter_description_t **d = nullptr;
    const int n = libvlc_media_player_get_full_chapter_descriptions(mp, -1, &d);
    libvlc_media_player_release(mp);
    int64_t base_ms;
    {
        std::lock_guard<std::mutex> lock(mu_);
        base_ms = base_us_ / 1000;
    }
    for (int i = 0; i < n; ++i) {
        ChapterMark c;
        c.name = d[i]->psz_name && *d[i]->psz_name ? d[i]->psz_name : "Chapter " + std::to_string(i + 1);
        c.start_ms = std::max<int64_t>(0, d[i]->i_time_offset - base_ms);
        out.push_back(std::move(c));
    }
    if (n > 0) libvlc_chapter_descriptions_release(d, static_cast<unsigned>(n));
    std::lock_guard<std::mutex> lock(player_mu_);
    if (!out.empty()) chapters_ = out;
    return out;
}

std::string VlcSource::ass_header(int id) const {
    std::lock_guard<std::mutex> lock(mu_);
    const auto it = seen_subtitles_.find(id);
    return it == seen_subtitles_.end() ? std::string() : it->second.ass_header;
}

std::string VlcSource::subtitle_codec(int id) const {
    std::lock_guard<std::mutex> lock(mu_);
    const auto it = seen_subtitles_.find(id);
    return it == seen_subtitles_.end() ? std::string() : fourcc_text(it->second.codec);
}

#else  // no libVLC in this build

VlcSource::VlcSource(libvlc_instance_t *vlc, std::vector<std::string> options)
    : vlc_(vlc), options_(std::move(options)) {}
VlcSource::~VlcSource() = default;
std::string VlcSource::audio_path_text() const { return "none"; }
std::string VlcSource::dovi_text() const { return {}; }
std::vector<media::FontAttachment> VlcSource::fonts() const { return {}; }
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
bool VlcSource::to_packet(Item &, media::Packet *) { return false; }
bool VlcSource::set_speed(double, std::string *why) {
    if (why != nullptr) *why = "this build has no libVLC";
    return false;
}
bool VlcSource::stretch_audio(media::Packet *) { return false; }
void VlcSource::land_seek_locked(int64_t) {}
void VlcSource::commit_seek_gop_locked() {}
libvlc_media_player_t *VlcSource::hold_player() const { return nullptr; }
std::vector<VlcSource::SubtitleTrack> VlcSource::subtitle_tracks() const { return {}; }
int VlcSource::subtitle_track() const { return -1; }
void VlcSource::set_subtitle_track(int) {}
bool VlcSource::add_subtitle_file(const std::string &) { return false; }
std::string VlcSource::ass_header(int) const { return {}; }
std::vector<VlcSource::ChapterMark> VlcSource::chapters() const { return {}; }
std::string VlcSource::subtitle_codec(int) const { return {}; }

#endif

}  // namespace vplayer
