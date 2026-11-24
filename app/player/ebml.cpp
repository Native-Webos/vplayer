// SPDX-License-Identifier: GPL-3.0-or-later
#include "player/ebml.hpp"

#include <algorithm>
#include <sys/types.h>

namespace vplayer::ebml {

bool read_at(std::FILE *f, uint64_t at, void *buf, size_t n) {
    if (fseeko(f, static_cast<off_t>(at), SEEK_SET) != 0) return false;
    return std::fread(buf, 1, n, f) == n;
}

bool header(std::FILE *f, uint64_t at, Element *out) {
    uint8_t b[12];
    if (fseeko(f, static_cast<off_t>(at), SEEK_SET) != 0) return false;
    const size_t got = std::fread(b, 1, sizeof b, f);
    if (got < 2) return false;
    int il = 1;
    while (il <= 4 && !(b[0] & (0x80 >> (il - 1)))) ++il;
    if (il > 4 || static_cast<size_t>(il) >= got) return false;
    uint32_t id = 0;
    for (int i = 0; i < il; ++i) id = id << 8 | b[i];
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
    out->id = id;
    out->size = all_ones ? kUnknownSize : v;
    out->data = at + static_cast<uint64_t>(il + sl);
    return true;
}

uint64_t read_uint(std::FILE *f, const Element &e) {
    uint8_t b[8] = {};
    if (e.size == 0 || e.size > 8 || !read_at(f, e.data, b, static_cast<size_t>(e.size))) return 0;
    uint64_t v = 0;
    for (uint64_t i = 0; i < e.size; ++i) v = v << 8 | b[i];
    return v;
}

std::string read_string(std::FILE *f, const Element &e, size_t max) {
    if (e.size == kUnknownSize || e.size > max) return {};
    std::string s(static_cast<size_t>(e.size), '\0');
    if (!s.empty() && !read_at(f, e.data, s.data(), s.size())) return {};
    while (!s.empty() && s.back() == '\0') s.pop_back();
    return s;
}

bool read_bytes(std::FILE *f, const Element &e, std::vector<uint8_t> *out, size_t max) {
    if (e.size == kUnknownSize || e.size > max) return false;
    out->resize(static_cast<size_t>(e.size));
    return out->empty() || read_at(f, e.data, out->data(), out->size());
}

bool segment(std::FILE *f, uint64_t file_size, uint64_t *data, uint64_t *end) {
    Element h, s;
    if (!header(f, 0, &h) || h.id != kHeader || h.size == kUnknownSize) return false;
    if (!header(f, h.end(), &s) || s.id != kSegment) return false;
    *data = s.data;
    *end = s.size == kUnknownSize ? file_size : std::min(file_size, s.end());
    return true;
}

}  // namespace vplayer::ebml
