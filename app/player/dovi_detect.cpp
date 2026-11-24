// SPDX-License-Identifier: GPL-3.0-or-later
#include "player/dovi_detect.hpp"

#include "player/ebml.hpp"

#include "wnk/media/dovi.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>

namespace vplayer {

namespace {

constexpr int kTransferPq = 16, kTransferHlg = 18;

struct FileCloser {
    void operator()(std::FILE *f) const { std::fclose(f); }
};
using File = std::unique_ptr<std::FILE, FileCloser>;

using ebml::read_at;

uint32_t be32(const uint8_t *p) { return uint32_t{p[0]} << 24 | uint32_t{p[1]} << 16 | uint32_t{p[2]} << 8 | p[3]; }

// ---- Matroska (EBML) ----

// The record in a TrackEntry's BlockAdditionMapping children, for a video track.
constexpr uint32_t kBlockAdditionMapping = 0x41E4, kBlockAddIdType = 0x41E7, kBlockAddIdExtraData = 0x41ED;

bool mkv_track(std::FILE *f, uint64_t at, uint64_t end, DoviConfig *out) {
    bool video = false;
    std::vector<uint8_t> record;
    while (at < end) {
        ebml::Element e;
        if (!ebml::header(f, at, &e) || e.size == ebml::kUnknownSize) return false;
        if (e.id == ebml::kTrackType) video = ebml::read_uint(f, e) == 1;
        if (e.id == kBlockAdditionMapping) {
            uint32_t type = 0;
            std::vector<uint8_t> extra;
            for (uint64_t m = e.data; m < e.end();) {
                ebml::Element c;
                if (!ebml::header(f, m, &c) || c.size == ebml::kUnknownSize) break;
                if (c.id == kBlockAddIdType) type = static_cast<uint32_t>(ebml::read_uint(f, c));
                if (c.id == kBlockAddIdExtraData && !ebml::read_bytes(f, c, &extra, 64)) extra.clear();
                m = c.end();
            }
            if ((type == 0x64766343 || type == 0x64767643) && !extra.empty()) record = extra;  // 'dvcC', 'dvvC'
        }
        at = e.end();
    }
    return video && !record.empty() && parse_dovi_record(record.data(), record.size(), out);
}

bool find_in_mkv(std::FILE *f, uint64_t file_size, DoviConfig *out) {
    uint64_t at, seg_end;
    if (!ebml::segment(f, file_size, &at, &seg_end)) return false;
    // Top-level children up to the first cluster: Tracks comes before it in practice.
    for (int guard = 0; guard < 64 && at < seg_end; ++guard) {
        ebml::Element e;
        if (!ebml::header(f, at, &e) || e.size == ebml::kUnknownSize || e.id == ebml::kCluster) return false;
        const uint64_t end = std::min(seg_end, e.end());
        if (e.id == ebml::kTracks) {
            for (uint64_t t = e.data; t < end;) {
                ebml::Element te;
                if (!ebml::header(f, t, &te) || te.size == ebml::kUnknownSize) return false;
                if (te.id == ebml::kTrackEntry && mkv_track(f, te.data, te.end(), out)) return true;
                t = te.end();
            }
            return false;
        }
        at = end;
    }
    return false;
}

// ---- MP4 (ISO BMFF) ----

// Calls `fn(type, data_start, data_end)` for each box in [at, end); stops when it returns true.
template <typename Fn>
bool mp4_boxes(std::FILE *f, uint64_t at, uint64_t end, Fn fn) {
    for (int guard = 0; guard < 4096 && at + 8 <= end; ++guard) {
        uint8_t h[16];
        if (!read_at(f, at, h, 8)) return false;
        uint64_t size = be32(h);
        uint64_t hl = 8;
        if (size == 1) {
            if (!read_at(f, at + 8, h + 8, 8)) return false;
            size = uint64_t{be32(h + 8)} << 32 | be32(h + 12);
            hl = 16;
        } else if (size == 0) {
            size = end - at;
        }
        if (size < hl || at + size > end) return false;
        if (fn(std::string(reinterpret_cast<const char *>(h + 4), 4), at + hl, at + size)) return true;
        at += size;
    }
    return false;
}

bool find_in_mp4(std::FILE *f, uint64_t file_size, DoviConfig *out) {
    uint8_t h[8];
    if (!read_at(f, 4, h, 4) || std::memcmp(h, "ftyp", 4) != 0) return false;
    bool found = false;
    auto in = [&](const char *name, auto inner) {
        return [&, name, inner](const std::string &t, uint64_t a, uint64_t e) {
            return t == name && mp4_boxes(f, a, e, inner);
        };
    };
    // stsd: a full box with an entry count, then sample entries; HEVC entries end their 78-byte
    // VisualSampleEntry with child boxes, among them dvcC/dvvC.
    auto stsd = [&](const std::string &t, uint64_t a, uint64_t e) {
        if (t != "stsd") return false;
        return mp4_boxes(f, a + 8, e, [&](const std::string &entry, uint64_t ea, uint64_t ee) {
            if (entry != "hvc1" && entry != "hev1" && entry != "dvh1" && entry != "dvhe") return false;
            return mp4_boxes(f, ea + 78, ee, [&](const std::string &c, uint64_t ca, uint64_t ce) {
                if (c != "dvcC" && c != "dvvC") return false;
                uint8_t rec[24] = {};
                const size_t n = static_cast<size_t>(std::min<uint64_t>(ce - ca, sizeof rec));
                found = read_at(f, ca, rec, n) && parse_dovi_record(rec, n, out);
                return found;
            });
        });
    };
    auto stbl = in("stbl", stsd);
    auto minf = in("minf", stbl);
    auto mdia = in("mdia", minf);
    auto trak = in("trak", mdia);
    auto moov = in("moov", trak);
    mp4_boxes(f, 0, file_size, moov);
    return found;
}

}  // namespace

bool parse_dovi_record(const uint8_t *d, size_t n, DoviConfig *out) {
    if (d == nullptr || n < 5) return false;
    DoviConfig c;
    c.profile = d[2] >> 1;
    c.level = (d[2] & 1) << 5 | d[3] >> 3;
    c.rpu = (d[3] & 4) != 0;
    c.el = (d[3] & 2) != 0;
    c.bl = (d[3] & 1) != 0;
    c.bl_compatibility = d[4] >> 4;
    *out = c;
    return true;
}

bool find_dovi_config(const std::string &path, DoviConfig *out) {
    File f(std::fopen(path.c_str(), "rb"));
    if (!f) return false;
    if (fseeko(f.get(), 0, SEEK_END) != 0) return false;
    const off_t size = ftello(f.get());
    if (size <= 8) return false;
    return find_in_mkv(f.get(), static_cast<uint64_t>(size), out) ||
           find_in_mp4(f.get(), static_cast<uint64_t>(size), out);
}

DoviNals scan_dovi_nals(const uint8_t *d, size_t n) {
    DoviNals r;
    for (size_t i = 0; i + 4 < n; ++i) {
        if (d[i] != 0 || d[i + 1] != 0 || d[i + 2] != 1) continue;
        const int type = (d[i + 3] >> 1) & 0x3F;
        const int layer = (d[i + 3] & 1) << 5 | d[i + 4] >> 3;
        if (type == 62) r.rpu = true;
        if (type == 63 || layer > 0) r.el = true;
        i += 2;
    }
    return r;
}

DoviPlan dovi_plan(const DoviConfig &rec, const DoviNals &nals, const wnk::media::VideoColour &colour,
                   bool can_convert) {
    DoviPlan p;
    const bool have_record = rec.profile >= 0;
    const bool rpu = have_record ? rec.rpu : nals.rpu;
    const bool el = have_record ? rec.el : nals.el;
    if (!rpu && !el) return p;
    if (el) {
        // Dual layer (Profile 7): the enhancement layer is always removed; RPUs are rewritten for
        // 8.1 when the base layer is HDR10 (compatibility 6) and libdovi is in the build.
        const int compat = have_record ? rec.bl_compatibility : colour.transfer == kTransferPq ? 6 : 0;
        const int profile = have_record ? rec.profile : 7;
        p.convert_p7 = true;
        if (can_convert && rpu && wnk::media::dovi_converts_to_p81(profile, true, compat)) {
            p.profile = 8;
            p.how = "Profile 7 dual layer, fed as 8.1";
        } else {
            p.how = "Profile " + std::to_string(profile) + " dual layer, fed as its base layer";
        }
        return p;
    }
    if (have_record) {
        p.profile = rec.profile;
        p.how = "Profile " + std::to_string(rec.profile) + (rec.profile == 8 ? "." + std::to_string(rec.bl_compatibility) : "") +
                " (configuration record)";
        return p;
    }
    // RPUs without a record: the base layer's signalling tells the profile.
    if (colour.transfer == kTransferPq) {
        p.profile = 8;
        p.how = "Profile 8.1 (RPUs, HDR10 base layer)";
    } else if (colour.transfer == kTransferHlg) {
        p.profile = 8;
        p.how = "Profile 8.4 (RPUs, HLG base layer)";
    } else {
        p.profile = 5;
        p.how = "Profile 5 (RPUs, no HDR10 or HLG base layer)";
    }
    return p;
}

}  // namespace vplayer
