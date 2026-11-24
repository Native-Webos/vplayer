// SPDX-License-Identifier: GPL-3.0-or-later
#include "player/subtitle_bitmap.hpp"

#include <algorithm>
#include <cstring>

namespace vplayer {

namespace media = wnk::media;

namespace {

constexpr uint32_t fourcc(char a, char b, char c, char d) {
    return static_cast<uint32_t>(static_cast<uint8_t>(a)) | static_cast<uint32_t>(static_cast<uint8_t>(b)) << 8 |
           static_cast<uint32_t>(static_cast<uint8_t>(c)) << 16 | static_cast<uint32_t>(static_cast<uint8_t>(d)) << 24;
}
constexpr uint32_t kPgs = fourcc('b', 'd', 'p', 'g');
constexpr uint32_t kDvd = fourcc('s', 'p', 'u', ' ');

int be16(const uint8_t *p) { return p[0] << 8 | p[1]; }
int be24(const uint8_t *p) { return p[0] << 16 | p[1] << 8 | p[2]; }

uint32_t pack(int r, int g, int b, int a) {
    auto c = [](int v) { return static_cast<uint32_t>(std::clamp(v, 0, 255)); };
    return c(r) | c(g) << 8 | c(b) << 16 | c(a) << 24;  // bytes R, G, B, A in memory (little endian)
}

// Bitmap of `w`×`h` RGBA pixels at (x, y), trimmed to its visible pixels; false if none are.
bool trimmed_bitmap(const std::vector<uint32_t> &px, int w, int h, int x, int y, media::SubtitleBitmap *out) {
    int x0 = w, y0 = h, x1 = -1, y1 = -1;
    for (int j = 0; j < h; ++j)
        for (int i = 0; i < w; ++i)
            if ((px[static_cast<size_t>(j) * w + i] >> 24) != 0) {
                x0 = std::min(x0, i);
                x1 = std::max(x1, i);
                y0 = std::min(y0, j);
                y1 = std::max(y1, j);
            }
    if (x1 < 0) return false;
    out->x = x + x0;
    out->y = y + y0;
    out->width = x1 - x0 + 1;
    out->height = y1 - y0 + 1;
    out->rgba.resize(static_cast<size_t>(out->width) * out->height * 4);
    for (int j = 0; j < out->height; ++j)
        std::memcpy(&out->rgba[static_cast<size_t>(j) * out->width * 4], &px[static_cast<size_t>(y0 + j) * w + x0],
                    static_cast<size_t>(out->width) * 4);
    return true;
}

// PGS object RLE (HDMV): a non-zero byte is one pixel; 0 then a flags byte: 0 ends the line,
// else bit 6 = 14-bit length, bit 7 = a colour byte follows (colour 0 otherwise).
bool pgs_rle(const std::vector<uint8_t> &rle, int w, int h, const std::array<uint32_t, 256> &pal,
             std::vector<uint32_t> *px) {
    px->assign(static_cast<size_t>(w) * h, 0);
    size_t i = 0;
    int x = 0, y = 0;
    while (i < rle.size() && y < h) {
        int color, run;
        const uint8_t b = rle[i++];
        if (b != 0) {
            color = b;
            run = 1;
        } else {
            if (i >= rle.size()) break;
            const uint8_t f = rle[i++];
            if (f == 0) {  // end of line
                x = 0;
                ++y;
                continue;
            }
            run = f & 0x3F;
            if (f & 0x40) {
                if (i >= rle.size()) break;
                run = run << 8 | rle[i++];
            }
            color = 0;
            if (f & 0x80) {
                if (i >= rle.size()) break;
                color = rle[i++];
            }
        }
        const uint32_t c = pal[static_cast<size_t>(color)];
        for (int k = 0; k < run && x < w; ++k) (*px)[static_cast<size_t>(y) * w + x++] = c;
    }
    return true;
}

// DVD 2-bit RLE, nibble-coded; one field's lines from `offset`.
struct NibbleReader {
    const uint8_t *d;
    size_t size, pos2;  // position in nibbles
    int next() {
        const size_t byte = pos2 / 2;
        if (byte >= size) return -1;
        const int v = (pos2 % 2 == 0) ? d[byte] >> 4 : d[byte] & 0x0F;
        ++pos2;
        return v;
    }
    void align() { pos2 = (pos2 + 1) & ~size_t{1}; }
};

void dvd_field(const uint8_t *d, size_t size, size_t offset, int w, int h, int first_line,
               const uint32_t colors[4], std::vector<uint32_t> *px) {
    NibbleReader r{d, size, offset * 2};
    for (int y = first_line; y < h; y += 2) {
        int x = 0;
        while (x < w) {
            int v = r.next();
            if (v < 0) return;
            if (v < 0x4) {
                v = v << 4 | std::max(0, r.next());
                if (v < 0x10) {
                    v = v << 4 | std::max(0, r.next());
                    if (v < 0x40) v = v << 4 | std::max(0, r.next());
                }
            }
            int run = v >> 2;
            if (run == 0) run = w - x;  // to the end of the line
            const uint32_t c = colors[v & 3];
            for (int k = 0; k < run && x < w; ++k) (*px)[static_cast<size_t>(y) * w + x++] = c;
        }
        r.align();
    }
}

}  // namespace

BitmapSubtitleDecoder::BitmapSubtitleDecoder(uint32_t f, const uint32_t *dvd_palette, int canvas_w, int canvas_h)
    : fourcc_(f), canvas_w_(canvas_w), canvas_h_(canvas_h) {
    if (dvd_palette != nullptr) {
        has_dvd_palette_ = true;
        std::copy(dvd_palette, dvd_palette + 16, dvd_palette_.begin());
    }
}

bool BitmapSubtitleDecoder::handles(uint32_t f) { return f == kPgs || f == kDvd; }

void BitmapSubtitleDecoder::set_video_size(int width, int height) {
    video_w_ = width;
    video_h_ = height;
}

uint32_t BitmapSubtitleDecoder::rgba(int y, int cb, int cr, int a, bool hd) const {
    const double yy = 1.164 * (y - 16), u = cb - 128, v = cr - 128;
    if (hd) return pack(static_cast<int>(yy + 1.793 * v + 0.5), static_cast<int>(yy - 0.213 * u - 0.533 * v + 0.5),
                        static_cast<int>(yy + 2.112 * u + 0.5), a);
    return pack(static_cast<int>(yy + 1.596 * v + 0.5), static_cast<int>(yy - 0.392 * u - 0.813 * v + 0.5),
                static_cast<int>(yy + 2.017 * u + 0.5), a);
}

std::vector<BitmapSubtitleDecoder::Cue> BitmapSubtitleDecoder::decode(const uint8_t *data, size_t size, int track,
                                                                     int64_t pts_us) {
    if (data == nullptr || size == 0 || pts_us == media::kNoTime) return {};
    return fourcc_ == kPgs ? decode_pgs(data, size, track, pts_us) : decode_dvd(data, size, track, pts_us);
}

std::vector<BitmapSubtitleDecoder::Cue> BitmapSubtitleDecoder::decode_pgs(const uint8_t *d, size_t n, int track,
                                                                         int64_t pts_us) {
    std::vector<Cue> out;
    size_t i = 0;
    while (i + 3 <= n) {
        const int type = d[i];
        const size_t len = static_cast<size_t>(be16(d + i + 1));
        const uint8_t *p = d + i + 3;
        if (i + 3 + len > n) break;
        i += 3 + len;
        switch (type) {
            case 0x16: {  // PCS: presentation composition
                if (len < 11) break;
                pgs_w_ = be16(p);
                pgs_h_ = be16(p + 2);
                const int state = p[7];
                pgs_palette_ = p[9];
                const int count = p[10];
                if (state & 0x80) {  // epoch start: nothing carries over
                    pgs_objects_.clear();
                    pgs_palettes_.clear();
                }
                pgs_placements_.clear();
                size_t k = 11;
                for (int o = 0; o < count && k + 8 <= len; ++o) {
                    PgsPlacement pl;
                    pl.object = be16(p + k);
                    pl.cropped = (p[k + 3] & 0x80) != 0;
                    pl.x = be16(p + k + 4);
                    pl.y = be16(p + k + 6);
                    k += 8;
                    if (pl.cropped && k + 8 <= len) {
                        pl.crop_x = be16(p + k);
                        pl.crop_y = be16(p + k + 2);
                        pl.crop_w = be16(p + k + 4);
                        pl.crop_h = be16(p + k + 6);
                        k += 8;
                    }
                    pgs_placements_.push_back(pl);
                }
                pgs_have_pcs_ = true;
                pgs_pts_ = pts_us;
                break;
            }
            case 0x14: {  // PDS: palette
                if (len < 2) break;
                auto &pal = pgs_palettes_[p[0]];
                const bool hd = pgs_h_ >= 720;
                for (size_t k = 2; k + 5 <= len; k += 5)
                    pal[p[k]] = rgba(p[k + 1], p[k + 3], p[k + 2], p[k + 4], hd);  // id, Y, Cr, Cb, A
                break;
            }
            case 0x15: {  // ODS: object (possibly in fragments)
                if (len < 4) break;
                auto &obj = pgs_objects_[be16(p)];
                const int seq = p[3];
                size_t k = 4;
                if (seq & 0x80) {  // first fragment
                    if (len < 11) break;
                    obj.expected = static_cast<size_t>(be24(p + 4)) - 4;  // the length counts width and height
                    obj.width = be16(p + 7);
                    obj.height = be16(p + 9);
                    obj.rle.clear();
                    k = 11;
                }
                obj.rle.insert(obj.rle.end(), p + k, p + len);
                break;
            }
            case 0x80: {  // END: the display set is complete
                if (!pgs_have_pcs_) break;
                pgs_have_pcs_ = false;
                auto cue = std::make_shared<media::SubtitleCue>();
                cue->track = track;
                cue->start_us = pgs_pts_ != media::kNoTime ? pgs_pts_ : pts_us;
                cue->canvas_w = pgs_w_;
                cue->canvas_h = pgs_h_;
                const auto pal = pgs_palettes_.find(pgs_palette_);
                for (const auto &pl : pgs_placements_) {
                    const auto obj = pgs_objects_.find(pl.object);
                    if (obj == pgs_objects_.end() || pal == pgs_palettes_.end() || obj->second.width <= 0 ||
                        obj->second.height <= 0)
                        continue;
                    std::vector<uint32_t> px;
                    pgs_rle(obj->second.rle, obj->second.width, obj->second.height, pal->second, &px);
                    int w = obj->second.width, h = obj->second.height, x = pl.x, y = pl.y;
                    if (pl.cropped && pl.crop_w > 0 && pl.crop_h > 0) {
                        // Keep the cropped window of the object, shown at the placement.
                        std::vector<uint32_t> c(static_cast<size_t>(pl.crop_w) * pl.crop_h, 0);
                        for (int j = 0; j < pl.crop_h && pl.crop_y + j < h; ++j)
                            for (int k2 = 0; k2 < pl.crop_w && pl.crop_x + k2 < w; ++k2)
                                c[static_cast<size_t>(j) * pl.crop_w + k2] =
                                    px[static_cast<size_t>(pl.crop_y + j) * w + pl.crop_x + k2];
                        px.swap(c);
                        w = pl.crop_w;
                        h = pl.crop_h;
                    }
                    media::SubtitleBitmap b;
                    if (trimmed_bitmap(px, w, h, x, y, &b)) cue->bitmaps.push_back(std::move(b));
                }
                out.push_back(std::move(cue));  // no bitmaps: clears the screen
                break;
            }
            default: break;  // WDS (windows) and others: positions come from the composition
        }
    }
    return out;
}

std::vector<BitmapSubtitleDecoder::Cue> BitmapSubtitleDecoder::decode_dvd(const uint8_t *d, size_t n, int track,
                                                                         int64_t pts_us) {
    if (n < 4) return {};
    const size_t total = std::min(n, static_cast<size_t>(be16(d)));
    size_t seq = static_cast<size_t>(be16(d + 2));
    int64_t start = media::kNoTime, stop = media::kNoTime;
    int x1 = 0, x2 = -1, y1 = 0, y2 = -1;
    size_t top = 0, bottom = 0;
    uint8_t color_idx[4] = {0, 1, 2, 3}, alpha[4] = {0, 15, 15, 15};
    // Control sequences: date (1024/90000 s units), next offset, then commands up to 0xFF.
    for (int guard = 0; guard < 16 && seq + 4 <= total; ++guard) {
        const int64_t date = static_cast<int64_t>(be16(d + seq)) * 1024 * 1000000 / 90000;
        const size_t next = static_cast<size_t>(be16(d + seq + 2));
        size_t k = seq + 4;
        bool end = false;
        while (k < total && !end) {
            switch (d[k++]) {
                case 0x00:  // forced start
                case 0x01: start = date; break;
                case 0x02:
                    // 0xFFFF is "no stop" in practice (ffmpeg writes it): the next subpicture replaces it.
                    if (be16(d + seq) != 0xFFFF) stop = date;
                    break;
                case 0x03:
                    if (k + 2 > total) return {};
                    color_idx[3] = d[k] >> 4, color_idx[2] = d[k] & 0x0F;
                    color_idx[1] = d[k + 1] >> 4, color_idx[0] = d[k + 1] & 0x0F;
                    k += 2;
                    break;
                case 0x04:
                    if (k + 2 > total) return {};
                    alpha[3] = d[k] >> 4, alpha[2] = d[k] & 0x0F;
                    alpha[1] = d[k + 1] >> 4, alpha[0] = d[k + 1] & 0x0F;
                    k += 2;
                    break;
                case 0x05:
                    if (k + 6 > total) return {};
                    x1 = d[k] << 4 | d[k + 1] >> 4;
                    x2 = (d[k + 1] & 0x0F) << 8 | d[k + 2];
                    y1 = d[k + 3] << 4 | d[k + 4] >> 4;
                    y2 = (d[k + 4] & 0x0F) << 8 | d[k + 5];
                    k += 6;
                    break;
                case 0x06:
                    if (k + 4 > total) return {};
                    top = static_cast<size_t>(be16(d + k));
                    bottom = static_cast<size_t>(be16(d + k + 2));
                    k += 4;
                    break;
                case 0xFF: end = true; break;
                default: end = true; break;  // unknown command: stop reading this sequence
            }
        }
        if (next == seq || next <= seq) break;
        seq = next;
    }
    const int w = x2 - x1 + 1, h = y2 - y1 + 1;
    auto cue = std::make_shared<media::SubtitleCue>();
    cue->track = track;
    cue->start_us = pts_us + (start != media::kNoTime ? start : 0);
    if (stop != media::kNoTime && stop > (start != media::kNoTime ? start : 0)) cue->end_us = pts_us + stop;
    cue->canvas_w = canvas_w_ > 0 ? canvas_w_ : video_w_ > 0 ? video_w_ : 720;
    cue->canvas_h = canvas_h_ > 0 ? canvas_h_ : video_h_ > 0 ? video_h_ : 576;
    if (w > 0 && h > 0 && w <= 4096 && h <= 4096 && top < total && bottom < total) {
        uint32_t colors[4];
        for (int c = 0; c < 4; ++c) {
            const int a = alpha[c] * 17;
            if (has_dvd_palette_) {
                const uint32_t e = dvd_palette_[color_idx[c]];
                colors[c] = rgba(static_cast<int>(e >> 16 & 0xFF), static_cast<int>(e >> 8 & 0xFF),
                                 static_cast<int>(e & 0xFF), a, false);
            } else {  // no palette: background, white text, black outline, grey anti-alias
                static const int grey[4] = {0, 235, 16, 128};
                colors[c] = rgba(grey[c], 128, 128, a, false);
            }
        }
        std::vector<uint32_t> px(static_cast<size_t>(w) * h, 0);
        dvd_field(d, total, top, w, h, 0, colors, &px);
        dvd_field(d, total, bottom, w, h, 1, colors, &px);
        media::SubtitleBitmap b;
        if (trimmed_bitmap(px, w, h, x1, y1, &b)) cue->bitmaps.push_back(std::move(b));
    }
    return {cue};
}

}  // namespace vplayer
