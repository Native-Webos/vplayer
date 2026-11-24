// SPDX-License-Identifier: GPL-3.0-or-later
#include "player/cea708.hpp"

#include <algorithm>

namespace vplayer {

namespace media = wnk::media;

namespace {

void utf8(std::string &out, char32_t c) {
    if (c < 0x80) {
        out += static_cast<char>(c);
    } else if (c < 0x800) {
        out += static_cast<char>(0xC0 | c >> 6);
        out += static_cast<char>(0x80 | (c & 0x3F));
    } else {
        out += static_cast<char>(0xE0 | c >> 12);
        out += static_cast<char>(0x80 | (c >> 6 & 0x3F));
        out += static_cast<char>(0x80 | (c & 0x3F));
    }
}

// G2's printable characters (after EXT1), the ones that have a Unicode equivalent.
char32_t g2(uint8_t c) {
    switch (c) {
        case 0x20: return U' ';       // transparent space
        case 0x21: return U' ';  // non-breaking transparent space
        case 0x25: return U'…';  // …
        case 0x2A: return U'Š';  // Š
        case 0x2C: return U'Œ';  // Œ
        case 0x30: return U'█';  // █
        case 0x31: return U'‘';  // ‘
        case 0x32: return U'’';  // ’
        case 0x33: return U'“';  // “
        case 0x34: return U'”';  // ”
        case 0x35: return U'•';  // •
        case 0x39: return U'™';  // ™
        case 0x3A: return U'š';  // š
        case 0x3C: return U'œ';  // œ
        case 0x3D: return U'℠';  // ℠
        case 0x3F: return U'Ÿ';  // Ÿ
        case 0x76: return U'⅛';  // ⅛
        case 0x77: return U'⅜';  // ⅜
        case 0x78: return U'⅝';  // ⅝
        case 0x79: return U'⅞';  // ⅞
        case 0x7A: return U'│';  // │
        case 0x7B: return U'┐';  // ┐
        case 0x7C: return U'└';  // └
        case 0x7D: return U'─';  // ─
        case 0x7E: return U'┘';  // ┘
        case 0x7F: return U'┌';  // ┌
        default: return 0;
    }
}

}  // namespace

void Cea708Decoder::reset() {
    buf_.clear();
    want_ = 0;
    win_ = {};
    cur_ = -1;
    changed_ = urgent_ = false;
    last_cue_us_ = media::kNoTime;
}

std::vector<Cea708Decoder::Cue> Cea708Decoder::feed(const Cc708Pair &pair, int64_t pts_us) {
    out_.clear();
    if (pair.start) {  // a packet starts: its header byte gives its size
        buf_.clear();
        const int code = pair.b1 & 0x3F;
        want_ = code == 0 ? 128 : static_cast<size_t>(code) * 2;
    } else if (want_ == 0) {
        return {};  // the middle of a packet whose start we did not see
    }
    buf_.push_back(pair.b1);
    buf_.push_back(pair.b2);
    if (buf_.size() >= want_) {
        buf_.resize(want_);
        packet(buf_, pts_us);
        buf_.clear();
        want_ = 0;
    }
    if (changed_ && (urgent_ || last_cue_us_ == media::kNoTime || pts_us - last_cue_us_ >= 250'000 ||
                     pts_us < last_cue_us_)) {
        changed_ = urgent_ = false;
        last_cue_us_ = pts_us;
        out_.push_back(cue(pts_us));
    }
    return std::move(out_);
}

std::vector<Cea708Decoder::Cue> Cea708Decoder::tick(int64_t pts_us) {
    if (!changed_ || (last_cue_us_ != media::kNoTime && pts_us - last_cue_us_ < 250'000 && pts_us >= last_cue_us_)) return {};
    changed_ = urgent_ = false;
    last_cue_us_ = pts_us;
    return {cue(pts_us)};
}

void Cea708Decoder::packet(const std::vector<uint8_t> &p, int64_t) {
    // Header (sequence, size), then service blocks: number (3 bits) and size (5 bits), number 7
    // extended by a byte. Only service 1 is ours.
    size_t i = 1;
    while (i < p.size()) {
        int number = p[i] >> 5;
        const size_t size = p[i] & 0x1F;
        ++i;
        if (number == 0) break;  // null block: padding to the end
        if (number == 7) {
            if (i >= p.size()) break;
            number = p[i++] & 0x3F;
        }
        if (i + size > p.size()) break;
        if (number == 1) service(p.data() + i, size);
        i += size;
    }
}

void Cea708Decoder::put(char32_t c) {
    Window *w = current();
    if (w == nullptr || !w->defined) return;
    if (w->row >= w->rows) w->row = w->rows - 1;
    auto &line = w->text[static_cast<size_t>(w->row)];
    if (line.size() < static_cast<size_t>(w->cols)) line.resize(static_cast<size_t>(w->cols), U' ');
    if (w->col >= w->cols) return;  // past the right edge: dropped (no word wrap)
    line[static_cast<size_t>(w->col++)] = c;
    if (w->visible) changed_ = true;
}

void Cea708Decoder::define(int id, const uint8_t *p) {
    Window &w = win_[static_cast<size_t>(id)];
    const bool was = w.defined;
    w.defined = true;
    w.visible = (p[0] & 0x20) != 0;
    w.priority = p[0] & 0x07;
    w.rows = (p[3] & 0x0F) + 1;
    w.cols = std::clamp((p[4] & 0x3F) + 1, 1, 42);
    if (!was) {
        w.row = w.col = 0;
        w.text.assign(static_cast<size_t>(w.rows), std::u32string());
    } else {
        w.text.resize(static_cast<size_t>(w.rows));
    }
    cur_ = id;
    changed_ = urgent_ = true;
}

void Cea708Decoder::service(const uint8_t *d, size_t n) {
    seen_ = true;
    for (size_t i = 0; i < n;) {
        const uint8_t c = d[i++];
        if (c >= 0x20 && c <= 0x7F) {  // G0
            put(c == 0x7F ? U'♪' : static_cast<char32_t>(c));
        } else if (c >= 0xA0) {  // G1: Latin-1
            put(static_cast<char32_t>(c));
        } else if (c < 0x20) {  // C0
            Window *w = current();
            switch (c) {
                case 0x08:  // BS
                    if (w != nullptr && w->defined && w->col > 0) {
                        --w->col;
                        auto &line = w->text[static_cast<size_t>(std::min(w->row, w->rows - 1))];
                        if (static_cast<size_t>(w->col) < line.size()) line[static_cast<size_t>(w->col)] = U' ';
                        if (w->visible) changed_ = true;
                    }
                    break;
                case 0x0C:  // FF: the window cleared, the pen to its start
                    if (w != nullptr && w->defined) {
                        for (auto &line : w->text) line.clear();
                        w->row = w->col = 0;
                        if (w->visible) changed_ = urgent_ = true;
                    }
                    break;
                case 0x0D:  // CR: the next row, scrolling up at the bottom
                    if (w != nullptr && w->defined) {
                        w->col = 0;
                        if (w->row + 1 < w->rows) {
                            ++w->row;
                        } else {
                            w->text.erase(w->text.begin());
                            w->text.emplace_back();
                        }
                        if (w->visible) changed_ = urgent_ = true;
                    }
                    break;
                case 0x0E:  // HCR: the row cleared, the pen to its start
                    if (w != nullptr && w->defined) {
                        w->text[static_cast<size_t>(std::min(w->row, w->rows - 1))].clear();
                        w->col = 0;
                        if (w->visible) changed_ = true;
                    }
                    break;
                case 0x10: {  // EXT1: the extended sets
                    if (i >= n) return;
                    const uint8_t e = d[i++];
                    if (e >= 0x20 && e <= 0x7F) {
                        if (const char32_t g = g2(e)) put(g);
                    } else if (e >= 0xA0) {
                        // G3: the [CC] icon (0xA0) only: not drawn
                    } else if (e < 0x08) {
                    } else if (e < 0x10) {
                        i += 1;
                    } else if (e < 0x18) {
                        i += 2;
                    } else if (e < 0x20) {
                        i += 3;
                    } else if (e >= 0x80 && e <= 0x87) {
                        i += 4;
                    } else if (e >= 0x88 && e <= 0x8F) {
                        i += 5;
                    } else if (e >= 0x90 && e <= 0x9F && i < n) {
                        i += 1 + (d[i] & 0x3F);  // variable length
                    }
                    break;
                }
                default:
                    if (c >= 0x11 && c <= 0x17) i += 1;       // P16 and reserved: one byte
                    else if (c >= 0x18 && c <= 0x1F) i += 2;  // two bytes
                    break;  // NUL, ETX
            }
        } else {  // C1: 0x80-0x9F
            if (c <= 0x87) {  // CWx
                if (win_[c & 7].defined) cur_ = c & 7;
            } else if (c >= 0x88 && c <= 0x8C) {  // CLW DSW HDW TGW DLW: a window bitmap
                if (i >= n) return;
                const uint8_t m = d[i++];
                for (int k = 0; k < 8; ++k) {
                    if (!(m & (1 << k))) continue;
                    Window &w = win_[static_cast<size_t>(k)];
                    if (!w.defined) continue;
                    if (c == 0x88) {
                        for (auto &line : w.text) line.clear();
                        if (w.visible) changed_ = urgent_ = true;
                    } else if (c == 0x89 || c == 0x8A || c == 0x8B) {
                        const bool v = c == 0x89 ? true : c == 0x8A ? false : !w.visible;
                        if (v != w.visible) changed_ = urgent_ = true;
                        w.visible = v;
                    } else {  // DLW
                        if (w.visible) changed_ = urgent_ = true;
                        w = Window{};
                        if (cur_ == k) cur_ = -1;
                    }
                }
            } else if (c == 0x8D) {  // DLY
                i += 1;
            } else if (c == 0x8E) {  // DLC
            } else if (c == 0x8F) {  // RST
                for (auto &w : win_)
                    if (w.visible) changed_ = urgent_ = true;
                win_ = {};
                cur_ = -1;
            } else if (c == 0x90) {  // SPA
                i += 2;
            } else if (c == 0x91) {  // SPC
                i += 3;
            } else if (c == 0x92) {  // SPL: the pen's row and column
                if (i + 2 > n) return;
                if (Window *w = current(); w != nullptr && w->defined) {
                    w->row = std::min(d[i] & 0x0F, w->rows - 1);
                    w->col = std::min(d[i + 1] & 0x3F, w->cols - 1);
                }
                i += 2;
            } else if (c == 0x97) {  // SWA
                i += 4;
            } else if (c >= 0x98) {  // DFx
                if (i + 6 > n) return;
                define(c & 7, d + i);
                i += 6;
            }
        }
    }
}

Cea708Decoder::Cue Cea708Decoder::cue(int64_t pts_us) const {
    auto c = std::make_shared<media::SubtitleCue>();
    c->track = track_;
    c->start_us = pts_us;
    // Visible windows, the highest priority (0) last so it reads nearest the bottom.
    std::vector<const Window *> shown;
    for (const auto &w : win_)
        if (w.defined && w.visible) shown.push_back(&w);
    std::stable_sort(shown.begin(), shown.end(), [](const Window *a, const Window *b) { return a->priority > b->priority; });
    for (const Window *w : shown) {
        for (const auto &row : w->text) {
            size_t a = 0, b = row.size();
            while (a < b && (row[a] == U' ' || row[a] == U' ')) ++a;
            while (b > a && (row[b - 1] == U' ' || row[b - 1] == U' ')) --b;
            if (a == b) continue;
            if (!c->text.empty()) c->text += '\n';
            for (size_t k = a; k < b; ++k) utf8(c->text, row[k]);
        }
    }
    return c;
}

}  // namespace vplayer
