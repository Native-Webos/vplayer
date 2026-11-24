// SPDX-License-Identifier: GPL-3.0-or-later
#include "player/cea608.hpp"

#include <algorithm>

namespace vplayer {

namespace media = wnk::media;

namespace {

// Annex-B NAL units of an access unit: (start of the NAL header, size).
std::vector<std::pair<const uint8_t *, size_t>> nals(const uint8_t *d, size_t n) {
    std::vector<std::pair<const uint8_t *, size_t>> out;
    size_t i = 0, start = std::string::npos;
    while (i + 3 <= n) {
        if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1) {
            if (start != std::string::npos) {
                size_t end = i;
                while (end > start && d[end - 1] == 0) --end;  // the next start code's leading zero
                out.emplace_back(d + start, end - start);
            }
            i += 3;
            start = i;
        } else {
            ++i;
        }
    }
    if (start != std::string::npos && start < n) out.emplace_back(d + start, n - start);
    return out;
}

// RBSP of a NAL unit: emulation-prevention bytes (00 00 03) removed.
std::vector<uint8_t> rbsp(const uint8_t *p, size_t n) {
    std::vector<uint8_t> out;
    out.reserve(n);
    int zeros = 0;
    for (size_t i = 0; i < n; ++i) {
        if (zeros >= 2 && p[i] == 3) {
            zeros = 0;
            continue;
        }
        zeros = p[i] == 0 ? zeros + 1 : 0;
        out.push_back(p[i]);
    }
    return out;
}

// cc_data of one user_data_registered_itu_t_t35 payload; field-1 pairs appended.
void parse_t35(const uint8_t *p, size_t n, std::vector<CcPair> *out) {
    if (n < 10 || p[0] != 0xB5 || p[1] != 0x00 || p[2] != 0x31 || p[3] != 'G' || p[4] != 'A' || p[5] != '9' ||
        p[6] != '4' || p[7] != 0x03)
        return;
    const int count = p[8] & 0x1F;
    if (!(p[8] & 0x40)) return;  // process_cc_data_flag
    size_t k = 10;               // after em_data
    for (int c = 0; c < count && k + 3 <= n; ++c, k += 3) {
        const bool valid = (p[k] & 0x04) != 0;
        const int type = p[k] & 0x03;
        if (valid && type == 0) out->emplace_back(p[k + 1], p[k + 2]);
    }
}

// The SEI messages of a NAL's RBSP (after its header): type 4 payloads to parse_t35.
void parse_sei(const std::vector<uint8_t> &r, size_t header, std::vector<CcPair> *out) {
    size_t i = header;
    while (i + 2 <= r.size()) {
        if (r[i] == 0x80 && i + 1 == r.size()) break;  // rbsp trailing bits
        int type = 0, size = 0;
        while (i < r.size() && r[i] == 0xFF) type += 255, ++i;
        if (i >= r.size()) break;
        type += r[i++];
        while (i < r.size() && r[i] == 0xFF) size += 255, ++i;
        if (i >= r.size()) break;
        size += r[i++];
        if (i + static_cast<size_t>(size) > r.size()) break;
        if (type == 4) parse_t35(r.data() + i, static_cast<size_t>(size), out);
        i += static_cast<size_t>(size);
    }
}

char32_t basic(uint8_t c) {
    switch (c) {
        case 0x2A: return U'á';  // á
        case 0x5C: return U'é';  // é
        case 0x5E: return U'í';  // í
        case 0x5F: return U'ó';  // ó
        case 0x60: return U'ú';  // ú
        case 0x7B: return U'ç';  // ç
        case 0x7C: return U'÷';  // ÷
        case 0x7D: return U'Ñ';  // Ñ
        case 0x7E: return U'ñ';  // ñ
        case 0x7F: return U'█';  // █
        default: return static_cast<char32_t>(c);
    }
}

const char32_t kSpecial[16] = {U'®', U'°', U'½', U'¿', U'™', U'¢', U'£', U'♪',
                               U'à', U' ',      U'è', U'â', U'ê', U'î', U'ô', U'û'};
// Extended Western European characters (first byte 0x12, then 0x13), 0x20..0x3F.
const char32_t kExt12[32] = {U'Á', U'É', U'Ó', U'Ú', U'Ü', U'ü', U'‘', U'¡',
                             U'*',      U'\'',     U'—', U'©', U'℠', U'•', U'“', U'”',
                             U'À', U'Â', U'Ç', U'È', U'Ê', U'Ë', U'ë', U'Î',
                             U'Ï', U'ï', U'Ô', U'Ù', U'ù', U'Û', U'«', U'»'};
const char32_t kExt13[32] = {U'Ã', U'ã', U'Í', U'Ì', U'ì', U'Ò', U'ò', U'Õ',
                             U'õ', U'{',      U'}',      U'\\',     U'^',      U'_',      U'|',      U'~',
                             U'Ä', U'ä', U'Ö', U'ö', U'ß', U'¥', U'¤', U'│',
                             U'Å', U'å', U'Ø', U'ø', U'┌', U'┐', U'└', U'┘'};

// Preamble address code rows, indexed by (first byte & 7) and bit 5 of the second.
const int kPacRow[8][2] = {{10, 0}, {0, 1}, {2, 3}, {11, 12}, {13, 14}, {4, 5}, {6, 7}, {8, 9}};

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

}  // namespace

std::vector<CcPair> cc608_pairs(const uint8_t *d, size_t n, bool hevc) {
    std::vector<CcPair> out;
    for (const auto &[p, size] : nals(d, n)) {
        if (size < 3) continue;
        const int type = hevc ? (p[0] >> 1) & 0x3F : p[0] & 0x1F;
        if ((!hevc && type != 6) || (hevc && type != 39)) continue;  // SEI (HEVC: prefix SEI)
        parse_sei(rbsp(p, size), hevc ? 2 : 1, &out);
    }
    return out;
}

void Cea608Decoder::reset() {
    shown_ = {};
    hidden_ = {};
    mode_ = Mode::PopOn;
    row_ = 14;
    col_ = 0;
    last_control_ = {0, 0};
    changed_ = urgent_ = false;
    last_cue_us_ = media::kNoTime;
}

void Cea608Decoder::put(char32_t c) {
    std::u32string &r = target()[static_cast<size_t>(row_)];
    if (r.size() < 32) r.resize(32, U' ');
    if (col_ > 31) col_ = 31;
    r[static_cast<size_t>(col_++)] = c;
    if (mode_ != Mode::PopOn) changed_ = true;
}

Cea608Decoder::Cue Cea608Decoder::cue(int64_t pts_us) const {
    auto c = std::make_shared<media::SubtitleCue>();
    c->track = track_;
    c->start_us = pts_us;
    for (const auto &row : shown_) {
        size_t a = 0, b = row.size();
        while (a < b && row[a] == U' ') ++a;
        while (b > a && row[b - 1] == U' ') --b;
        if (a == b) continue;
        if (!c->text.empty()) c->text += '\n';
        for (size_t i = a; i < b; ++i) utf8(c->text, row[i]);
    }
    return c;
}

std::vector<Cea608Decoder::Cue> Cea608Decoder::feed(uint8_t b1, uint8_t b2, int64_t pts_us) {
    std::vector<Cue> out;
    const uint8_t c1 = b1 & 0x7F, c2 = b2 & 0x7F;  // odd parity in bit 7
    if (c1 == 0 && c2 == 0) return out;            // padding
    if (c1 >= 0x10 && c1 <= 0x1F) {
        // A control code; sent twice in a row, the copy is skipped.
        if (last_control_ == CcPair{c1, c2}) {
            last_control_ = {0, 0};
            return out;
        }
        last_control_ = {c1, c2};
        if (c1 & 0x08) return out;  // data channel 2 (CC2): not ours
        const uint8_t cc = c1 & 0x07;
        if ((cc == 4 || cc == 5) && c2 >= 0x20 && c2 <= 0x2F) {  // miscellaneous control codes
            switch (c2) {
                case 0x20: mode_ = Mode::PopOn; break;  // resume caption loading
                case 0x21:                               // backspace
                    if (col_ > 0) {
                        --col_;
                        auto &r = target()[static_cast<size_t>(row_)];
                        if (static_cast<size_t>(col_) < r.size()) r[static_cast<size_t>(col_)] = U' ';
                        if (mode_ != Mode::PopOn) changed_ = true;
                    }
                    break;
                case 0x24: {  // delete to end of row
                    auto &r = target()[static_cast<size_t>(row_)];
                    if (static_cast<size_t>(col_) < r.size()) r.resize(static_cast<size_t>(col_));
                    if (mode_ != Mode::PopOn) changed_ = true;
                    break;
                }
                case 0x25:
                case 0x26:
                case 0x27:  // roll-up captions, 2-4 rows
                    rollup_rows_ = c2 - 0x23;
                    if (mode_ != Mode::RollUp) {
                        mode_ = Mode::RollUp;
                        shown_ = {};
                        changed_ = true;
                    }
                    row_ = 14;
                    col_ = 0;
                    break;
                case 0x29: mode_ = Mode::PaintOn; break;  // resume direct captioning
                case 0x2C:                                // erase displayed memory
                    shown_ = {};
                    changed_ = urgent_ = true;
                    break;
                case 0x2D:  // carriage return: roll up
                    if (mode_ == Mode::RollUp) {
                        const int top = std::max(0, row_ - rollup_rows_ + 1);
                        for (int r = top; r < row_; ++r) shown_[static_cast<size_t>(r)] = shown_[static_cast<size_t>(r + 1)];
                        for (int r = 0; r < top; ++r) shown_[static_cast<size_t>(r)].clear();
                        shown_[static_cast<size_t>(row_)].clear();
                        changed_ = urgent_ = true;
                    }
                    col_ = 0;
                    break;
                case 0x2E: hidden_ = {}; break;  // erase non-displayed memory
                case 0x2F:                       // end of caption: the loaded caption shows
                    std::swap(shown_, hidden_);
                    mode_ = Mode::PopOn;
                    changed_ = urgent_ = true;
                    break;
                default: break;  // flash, text restart, alarm codes
            }
        } else if (cc == 7 && c2 >= 0x21 && c2 <= 0x23) {  // tab offsets
            col_ = std::min(31, col_ + (c2 - 0x20));
        } else if (cc == 1 && c2 >= 0x20 && c2 <= 0x2F) {  // mid-row code: a space
            put(U' ');
        } else if (cc == 1 && c2 >= 0x30 && c2 <= 0x3F) {  // special characters (sent twice too)
            put(kSpecial[c2 - 0x30]);
        } else if ((cc == 2 || cc == 3) && c2 >= 0x20 && c2 <= 0x3F) {  // extended: replaces the previous
            if (col_ > 0) --col_;
            put(cc == 2 ? kExt12[c2 - 0x20] : kExt13[c2 - 0x20]);
        } else if (c2 >= 0x40 && c2 <= 0x7F) {  // preamble address code: row and indent
            const int row = kPacRow[cc][(c2 & 0x20) ? 1 : 0];
            if (mode_ == Mode::RollUp && row != row_) {
                // The base row moves: keep the rows above it.
                const int shift = row - row_;
                Screen moved{};
                for (int r = 0; r < 15; ++r) {
                    const int to = r + shift;
                    if (to >= 0 && to < 15) moved[static_cast<size_t>(to)] = shown_[static_cast<size_t>(r)];
                }
                shown_ = moved;
            }
            row_ = row;
            col_ = (c2 & 0x10) ? ((c2 & 0x0E) >> 1) * 4 : 0;
        }
    } else {
        last_control_ = {0, 0};
        if (c1 >= 0x20) put(basic(c1));
        if (c2 >= 0x20) put(basic(c2));
    }
    // Roll-up and paint-on change with every pair: one cue per 300 ms at most, except for what must
    // show at once (a caption swapped in, a clear, a new line).
    if (changed_ && (urgent_ || last_cue_us_ == media::kNoTime || pts_us - last_cue_us_ >= 300'000 || pts_us < last_cue_us_)) {
        changed_ = urgent_ = false;
        last_cue_us_ = pts_us;
        out.push_back(cue(pts_us));
    }
    return out;
}

}  // namespace vplayer
