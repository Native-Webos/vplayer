// SPDX-License-Identifier: GPL-3.0-or-later
#include "player/teletext.hpp"

namespace vplayer {

namespace media = wnk::media;

namespace {

// The 13 characters each national option replaces, at 0x23 0x24 0x40 0x5B 0x5C 0x5D 0x5E 0x5F 0x60
// 0x7B 0x7C 0x7D 0x7E; indexed by the header's C12-C14 bits as VLC's telx decoder does (whose
// tables these are).
const uint16_t kNational[8][13] = {
    {0x00a3, 0x0024, 0x0040, 0x00ab, 0x00bd, 0x00bb, 0x005e, 0x0023, 0x002d, 0x00bc, 0x00a6, 0x00be, 0x00f7},  // English
    {0x00e9, 0x00ef, 0x00e0, 0x00eb, 0x00ea, 0x00f9, 0x00ee, 0x0023, 0x00e8, 0x00e2, 0x00f4, 0x00fb, 0x00e7},  // French
    {0x0023, 0x00a4, 0x00c9, 0x00c4, 0x00d6, 0x00c5, 0x00dc, 0x005f, 0x00e9, 0x00e4, 0x00f6, 0x00e5, 0x00fc},  // Swedish, Finnish, Hungarian
    {0x0023, 0x016f, 0x010d, 0x0165, 0x017e, 0x00fd, 0x00ed, 0x0159, 0x00e9, 0x00e1, 0x011b, 0x00fa, 0x0161},  // Czech, Slovak
    {0x0023, 0x0024, 0x00a7, 0x00c4, 0x00d6, 0x00dc, 0x005e, 0x005f, 0x00b0, 0x00e4, 0x00f6, 0x00fc, 0x00df},  // German
    {0x00e7, 0x0024, 0x00a1, 0x00e1, 0x00e9, 0x00ed, 0x00f3, 0x00fa, 0x00bf, 0x00fc, 0x00f1, 0x00e8, 0x00e0},  // Portuguese, Spanish
    {0x00a3, 0x0024, 0x00e9, 0x00b0, 0x00e7, 0x00bb, 0x005e, 0x0023, 0x00f9, 0x00e0, 0x00f2, 0x00e8, 0x00ec},  // Italian
    {0x0023, 0x00a4, 0x0162, 0x00c2, 0x015e, 0x0102, 0x00ce, 0x0131, 0x0163, 0x00e2, 0x015f, 0x0103, 0x00ee},  // Romanian
};

int national_slot(int ch) {
    static const int at[13] = {0x23, 0x24, 0x40, 0x5B, 0x5C, 0x5D, 0x5E, 0x5F, 0x60, 0x7B, 0x7C, 0x7D, 0x7E};
    for (int i = 0; i < 13; ++i)
        if (at[i] == ch) return i;
    return -1;
}

void put_utf8(std::string &out, uint32_t cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | cp >> 6);
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xE0 | cp >> 12);
        out += static_cast<char>(0x80 | (cp >> 6 & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

std::string trim(const std::string &s) {
    size_t a = 0, b = s.size();
    while (a < b && s[a] == ' ') ++a;
    while (b > a && s[b - 1] == ' ') --b;
    return s.substr(a, b - a);
}

// One row's 40 characters: the boxed text if the row has a box, control codes as spaces, the
// national option's letters where it has them.
std::string row_text(const uint8_t *c, int national) {
    std::string all, boxed;
    bool in_box = false, has_box = false;
    for (int i = 0; i < 40; ++i) {
        const int ch = c[i] & 0x7F;  // odd parity in bit 7
        if (ch == 0x0B || ch == 0x0A) {  // start box, end box
            in_box = ch == 0x0B;
            has_box = has_box || in_box;
            all += ' ';
            boxed += ' ';
            continue;
        }
        std::string out;
        const int slot = national_slot(ch);
        if (ch < 0x20 || ch == 0x7F) {
            out = " ";
        } else if (slot >= 0) {
            put_utf8(out, kNational[national & 7][slot]);
        } else {
            out = std::string(1, static_cast<char>(ch));
        }
        all += out;
        boxed += in_box ? out : " ";
    }
    // Collapse runs of spaces left by control codes.
    const std::string &s = has_box ? boxed : all;
    std::string r;
    for (const char ch : s)
        if (!(ch == ' ' && !r.empty() && r.back() == ' ')) r += ch;
    return trim(r);
}

}  // namespace

uint8_t TeletextDecoder::reverse(uint8_t b) {
    b = static_cast<uint8_t>((b & 0xF0) >> 4 | (b & 0x0F) << 4);
    b = static_cast<uint8_t>((b & 0xCC) >> 2 | (b & 0x33) << 2);
    return static_cast<uint8_t>((b & 0xAA) >> 1 | (b & 0x55) << 1);
}

int TeletextDecoder::unham(uint8_t b) { return (b >> 1 & 1) | (b >> 2 & 2) | (b >> 3 & 4) | (b >> 4 & 8); }

TeletextDecoder::TeletextDecoder(int magazine, int page) : magazine_(magazine), page_(page) {}

TeletextDecoder::Cue TeletextDecoder::cue(int track, int64_t pts_us) const {
    auto c = std::make_shared<media::SubtitleCue>();
    c->track = track;
    c->start_us = pts_us;
    for (const auto &r : rows_) {
        if (r.empty()) continue;
        if (!c->text.empty()) c->text += '\n';
        c->text += r;
    }
    return c;  // no text: clears
}

std::vector<TeletextDecoder::Cue> TeletextDecoder::decode(const uint8_t *d, size_t n, int track, int64_t pts_us) {
    std::vector<Cue> out;
    if (n < 1 || d[0] < 0x10 || d[0] > 0x1F || pts_us == media::kNoTime) return out;  // EBU data_identifier
    for (size_t i = 1; i + 2 <= n;) {
        const int id = d[i];
        const size_t len = d[i + 1];
        const uint8_t *u = d + i + 2;
        i += 2 + len;
        if (i > n) break;
        if ((id != 0x02 && id != 0x03) || len != 0x2C) continue;  // teletext (non-)subtitle data units
        uint8_t b[44];
        b[0] = u[0];
        for (int k = 1; k < 44; ++k) b[k] = reverse(u[k]);
        if (b[1] != 0xE4) continue;  // framing code
        const int addr = unham(b[2]) | unham(b[3]) << 4;
        const int mag = (addr & 7) == 0 ? 8 : (addr & 7);
        const int row = addr >> 3;
        if (row == 0) {  // page header
            const int page = unham(b[5]) << 4 | unham(b[4]);
            const bool subtitle = (unham(b[9]) & 8) != 0;
            if (magazine_ < 0 && subtitle) {  // no page named: the first subtitle page
                magazine_ = mag;
                page_ = page;
            }
            if (mag == magazine_) {  // a header in the magazine ends the page before it
                on_page_ = page == page_;
                if (on_page_) national_ = unham(b[11]) >> 1 & 7;  // C12-C14: the national option
                if (on_page_) {
                    // Subtitle pages are sent whole each time (erase or not): start from blank rows.
                    rows_.fill(std::string());
                    pending_ = true;
                }
            }
            continue;
        }
        if (row > 23 || !on_page_ || mag != magazine_) continue;
        rows_[static_cast<size_t>(row)] = row_text(b + 4, national_);
        pending_ = true;
    }
    if (pending_) {
        out.push_back(cue(track, pts_us));
        pending_ = false;
    }
    return out;
}

}  // namespace vplayer
