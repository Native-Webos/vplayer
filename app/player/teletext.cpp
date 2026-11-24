// SPDX-License-Identifier: GPL-3.0-or-later
#include "player/teletext.hpp"

namespace vplayer {

namespace media = wnk::media;

namespace {

std::string trim(const std::string &s) {
    size_t a = 0, b = s.size();
    while (a < b && s[a] == ' ') ++a;
    while (b > a && s[b - 1] == ' ') --b;
    return s.substr(a, b - a);
}

// One row's 40 characters: the boxed text if the row has a box, control codes as spaces.
std::string row_text(const uint8_t *c) {
    std::string all, boxed;
    bool in_box = false, has_box = false;
    for (int i = 0; i < 40; ++i) {
        const int ch = c[i] & 0x7F;  // odd parity in bit 7
        if (ch == 0x0B) {            // start box
            in_box = has_box = true;
            boxed += ' ';
            all += ' ';
            continue;
        }
        if (ch == 0x0A) {  // end box
            in_box = false;
            boxed += ' ';
            all += ' ';
            continue;
        }
        const char out = ch < 0x20 || ch == 0x7F ? ' ' : static_cast<char>(ch);
        all += out;
        boxed += in_box ? out : ' ';
    }
    // Collapse runs of spaces left by control codes.
    std::string s = has_box ? boxed : all, r;
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
                if (on_page_) {
                    // Subtitle pages are sent whole each time (erase or not): start from blank rows.
                    rows_.fill(std::string());
                    pending_ = true;
                }
            }
            continue;
        }
        if (row > 23 || !on_page_ || mag != magazine_) continue;
        rows_[static_cast<size_t>(row)] = row_text(b + 4);
        pending_ = true;
    }
    if (pending_) {
        out.push_back(cue(track, pts_us));
        pending_ = false;
    }
    return out;
}

}  // namespace vplayer
