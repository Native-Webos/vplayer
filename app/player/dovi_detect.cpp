// SPDX-License-Identifier: GPL-3.0-or-later
#include "player/dovi_detect.hpp"

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

bool read_at(std::FILE *f, uint64_t at, void *buf, size_t n) {
    if (fseeko(f, static_cast<off_t>(at), SEEK_SET) != 0) return false;
    return std::fread(buf, 1, n, f) == n;
}

uint32_t be32(const uint8_t *p) { return uint32_t{p[0]} << 24 | uint32_t{p[1]} << 16 | uint32_t{p[2]} << 8 | p[3]; }

// ---- Matroska (EBML) ----

constexpr uint32_t kSegment = 0x18538067, kTracks = 0x1654AE6B, kTrackEntry = 0xAE, kTrackType = 0x83,
                   kCluster = 0x1F43B675, kBlockAdditionMapping = 0x41E4, kBlockAddIdType = 0x41E7,
                   kBlockAddIdExtraData = 0x41ED;
constexpr uint64_t kUnknownSize = ~uint64_t{0};

// An element header at `at`: id (with its marker bits), data size, header length.
bool ebml_header(std::FILE *f, uint64_t at, uint32_t *id, uint64_t *size, int *len) {
    uint8_t b[12];
    if (fseeko(f, static_cast<off_t>(at), SEEK_SET) != 0) return false;
    const size_t got = std::fread(b, 1, sizeof b, f);
    if (got < 2) return false;
    int il = 1;
    while (il <= 4 && !(b[0] & (0x80 >> (il - 1)))) ++il;
    if (il > 4 || static_cast<size_t>(il) >= got) return false;
    *id = 0;
    for (int i = 0; i < il; ++i) *id = *id << 8 | b[i];
    const uint8_t s0 = b[il];
    int sl = 1;
    while (sl <= 8 && !(s0 & (0x80 >> (sl - 1)))) ++sl;
    if (sl > 8 || static_cast<size_t>(il + sl) > got) return false;
    uint64_t v = s0 & (0xFF >> sl);
    bool all_ones = v == (0xFFu >> sl);
    for (int i = 1; i < sl; ++i) {
        v = v << 8 | b[il + i];
        all_ones = all_ones && b[il + i] == 0xFF;
    }
    *size = all_ones ? kUnknownSize : v;
    *len = il + sl;
    return true;
}

uint64_t ebml_uint(std::FILE *f, uint64_t at, uint64_t size) {
    uint8_t b[8] = {};
    if (size == 0 || size > 8 || !read_at(f, at, b, static_cast<size_t>(size))) return 0;
    uint64_t v = 0;
    for (uint64_t i = 0; i < size; ++i) v = v << 8 | b[i];
    return v;
}

// The record in a TrackEntry's BlockAdditionMapping children, for a video track.
bool mkv_track(std::FILE *f, uint64_t at, uint64_t end, DoviConfig *out) {
    bool video = false;
    std::vector<uint8_t> record;
    while (at < end) {
        uint32_t id;
        uint64_t size;
        int len;
        if (!ebml_header(f, at, &id, &size, &len) || size == kUnknownSize) return false;
        const uint64_t data = at + len;
        if (id == kTrackType) video = ebml_uint(f, data, size) == 1;
        if (id == kBlockAdditionMapping) {
            uint64_t m = data;
            uint32_t type = 0;
            std::vector<uint8_t> extra;
            while (m < data + size) {
                uint32_t cid;
                uint64_t csize;
                int clen;
                if (!ebml_header(f, m, &cid, &csize, &clen) || csize == kUnknownSize) break;
                if (cid == kBlockAddIdType) type = static_cast<uint32_t>(ebml_uint(f, m + clen, csize));
                if (cid == kBlockAddIdExtraData && csize <= 64) {
                    extra.resize(static_cast<size_t>(csize));
                    if (!read_at(f, m + clen, extra.data(), extra.size())) extra.clear();
                }
                m += clen + csize;
            }
            if ((type == 0x64766343 || type == 0x64767643) && !extra.empty()) record = extra;  // 'dvcC', 'dvvC'
        }
        at = data + size;
    }
    return video && !record.empty() && parse_dovi_record(record.data(), record.size(), out);
}

bool find_in_mkv(std::FILE *f, uint64_t file_size, DoviConfig *out) {
    uint32_t id;
    uint64_t size;
    int len;
    uint64_t at = 0;
    if (!ebml_header(f, 0, &id, &size, &len) || id != 0x1A45DFA3) return false;  // EBML header
    at = len + size;
    if (!ebml_header(f, at, &id, &size, &len) || id != kSegment) return false;
    at += len;
    const uint64_t seg_end = size == kUnknownSize ? file_size : std::min(file_size, at + size);
    // Top-level children up to the first cluster: Tracks comes before it in practice.
    for (int guard = 0; guard < 64 && at < seg_end; ++guard) {
        if (!ebml_header(f, at, &id, &size, &len) || size == kUnknownSize || id == kCluster) return false;
        const uint64_t data = at + len, end = std::min(seg_end, data + size);
        if (id == kTracks) {
            for (uint64_t t = data; t < end;) {
                uint32_t tid;
                uint64_t tsize;
                int tlen;
                if (!ebml_header(f, t, &tid, &tsize, &tlen) || tsize == kUnknownSize) return false;
                if (tid == kTrackEntry && mkv_track(f, t + tlen, t + tlen + tsize, out)) return true;
                t += tlen + tsize;
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
