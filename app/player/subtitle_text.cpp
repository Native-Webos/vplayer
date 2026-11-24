// SPDX-License-Identifier: GPL-3.0-or-later
#include "player/subtitle_text.hpp"

#include "wnk/media/subtitles.hpp"
#include "wnk/media/webvtt.hpp"

#include <cctype>
#include <cstring>
#include <strings.h>

namespace vplayer {

namespace media = wnk::media;

namespace {

constexpr uint32_t fourcc(char a, char b, char c, char d) {
    return static_cast<uint32_t>(static_cast<uint8_t>(a)) | static_cast<uint32_t>(static_cast<uint8_t>(b)) << 8 |
           static_cast<uint32_t>(static_cast<uint8_t>(c)) << 16 | static_cast<uint32_t>(static_cast<uint8_t>(d)) << 24;
}
constexpr uint32_t kSubt = fourcc('s', 'u', 'b', 't');
constexpr uint32_t kSsa = fourcc('s', 's', 'a', ' ');
constexpr uint32_t kTx3g = fourcc('t', 'x', '3', 'g');
constexpr uint32_t kWebVtt = fourcc('w', 'v', 't', 't');

bool valid_utf8(const std::string &s) {
    size_t i = 0;
    while (i < s.size()) {
        const auto c = static_cast<uint8_t>(s[i]);
        size_t more;  // continuation bytes
        if (c < 0x80) {
            more = 0;
        } else if (c >= 0xC2 && c < 0xE0) {
            more = 1;
        } else if (c >= 0xE0 && c < 0xF0) {
            more = 2;
        } else if (c >= 0xF0 && c < 0xF5) {
            more = 3;
        } else {
            return false;
        }
        if (i + more >= s.size()) return false;  // truncated sequence
        for (size_t k = 1; k <= more; ++k)
            if ((static_cast<uint8_t>(s[i + k]) & 0xC0) != 0x80) return false;
        i += more + 1;
    }
    return true;
}

void put_utf8(std::string &out, uint32_t cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | cp >> 6);
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | cp >> 12);
        out += static_cast<char>(0x80 | (cp >> 6 & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | cp >> 18);
        out += static_cast<char>(0x80 | (cp >> 12 & 0x3F));
        out += static_cast<char>(0x80 | (cp >> 6 & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

// Windows-1252 (a superset of ISO-8859-1 for printable text) → UTF-8.
std::string cp1252_to_utf8(const std::string &s) {
    static const uint16_t high[32] = {0x20AC, 0xFFFD, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
                                      0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0xFFFD, 0x017D, 0xFFFD,
                                      0xFFFD, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
                                      0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0xFFFD, 0x017E, 0x0178};
    std::string out;
    out.reserve(s.size() + s.size() / 4);
    for (const char ch : s) {
        const auto c = static_cast<uint8_t>(ch);
        put_utf8(out, c >= 0x80 && c < 0xA0 ? high[c - 0x80] : c);
    }
    return out;
}

std::string utf16_to_utf8(const uint8_t *p, size_t n, bool big_endian) {
    std::string out;
    for (size_t i = 0; i + 1 < n; i += 2) {
        uint32_t u = big_endian ? (p[i] << 8 | p[i + 1]) : (p[i + 1] << 8 | p[i]);
        if (u >= 0xD800 && u < 0xDC00 && i + 3 < n) {
            const uint32_t lo = big_endian ? (p[i + 2] << 8 | p[i + 3]) : (p[i + 3] << 8 | p[i + 2]);
            if (lo >= 0xDC00 && lo < 0xE000) {
                u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00);
                i += 2;
            }
        }
        put_utf8(out, u);
    }
    return out;
}

// CRLF → LF, literal \N and \n (ASS line breaks that leak into SubRip) → LF, each line trimmed,
// blank lines dropped.
std::string tidy_lines(const std::string &s) {
    std::string flat;
    flat.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\r') continue;
        if (s[i] == '\\' && i + 1 < s.size() && (s[i + 1] == 'N' || s[i + 1] == 'n')) {
            flat += '\n';
            ++i;
            continue;
        }
        flat += s[i];
    }
    std::string out;
    size_t start = 0;
    while (start <= flat.size()) {
        size_t end = flat.find('\n', start);
        if (end == std::string::npos) end = flat.size();
        size_t a = start, b = end;
        while (a < b && std::isspace(static_cast<unsigned char>(flat[a]))) ++a;
        while (b > a && std::isspace(static_cast<unsigned char>(flat[b - 1]))) --b;
        if (b > a) {
            if (!out.empty()) out += '\n';
            out.append(flat, a, b - a);
        }
        start = end + 1;
    }
    return out;
}

uint32_t be32(const uint8_t *p) { return uint32_t{p[0]} << 24 | uint32_t{p[1]} << 16 | uint32_t{p[2]} << 8 | p[3]; }

// WebVTT sample (ISO/IEC 14496-30): the cue payloads of every vttc box, one per line group.
bool wvtt_text(const uint8_t *d, size_t n, std::string *out) {
    out->clear();
    size_t i = 0;
    while (i + 8 <= n) {
        const uint32_t size = be32(d + i);
        if (size < 8 || i + size > n) return false;
        if (std::memcmp(d + i + 4, "vttc", 4) == 0) {
            size_t j = i + 8;
            while (j + 8 <= i + size) {
                const uint32_t cs = be32(d + j);
                if (cs < 8 || j + cs > i + size) return false;
                if (std::memcmp(d + j + 4, "payl", 4) == 0) {
                    if (!out->empty()) *out += '\n';
                    out->append(reinterpret_cast<const char *>(d + j + 8), cs - 8);
                }
                j += cs;
            }
        }
        i += size;  // vtte (an empty cue) and anything else: nothing to show
    }
    return true;
}

}  // namespace

media::SubtitleKind subtitle_kind(uint32_t f) {
    switch (f) {
        case kSubt:
        case kSsa:
        case kTx3g:
        case kWebVtt: return media::SubtitleKind::Text;
        default: return media::SubtitleKind::None;
    }
}

std::string subtitle_utf8(const std::string &text, const char *encoding) {
    if (encoding != nullptr && (strcasecmp(encoding, "UTF-16") == 0 || strcasecmp(encoding, "UTF-16BE") == 0 ||
                                strcasecmp(encoding, "UTF-16LE") == 0)) {
        const auto *p = reinterpret_cast<const uint8_t *>(text.data());
        return utf16_to_utf8(p, text.size(), strcasecmp(encoding, "UTF-16LE") != 0);
    }
    if (valid_utf8(text)) return text;
    return cp1252_to_utf8(text);
}

std::string subrip_plain_text(const std::string &text) {
    std::string no_overrides;
    no_overrides.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '{' && i + 1 < text.size() && text[i + 1] == '\\') {
            const size_t close = text.find('}', i);
            if (close != std::string::npos) {
                i = close;
                continue;
            }
        }
        no_overrides += text[i];
    }
    return tidy_lines(media::webvtt_plain_text(tidy_lines(no_overrides)));
}

std::shared_ptr<const media::SubtitleCue> text_subtitle_cue(uint32_t f, const uint8_t *data, size_t size,
                                                            const char *encoding, int track, int64_t start_us,
                                                            int64_t length_us) {
    if (subtitle_kind(f) != media::SubtitleKind::Text || start_us == media::kNoTime) return nullptr;
    auto cue = std::make_shared<media::SubtitleCue>();
    cue->track = track;
    cue->start_us = start_us;
    cue->end_us = length_us > 0 ? start_us + length_us : media::kNoTime;
    // Demuxers often end text with a NUL.
    while (size > 0 && data[size - 1] == 0) --size;
    const std::string raw(reinterpret_cast<const char *>(data), size);

    if (f == kSubt) {
        cue->text = subrip_plain_text(subtitle_utf8(raw, encoding));
    } else if (f == kSsa) {
        const std::string event = subtitle_utf8(raw, encoding);
        cue->text = tidy_lines(media::ass_event_text(event));
        if (!event.empty()) cue->ass_events.push_back(event);
    } else if (f == kTx3g) {
        if (size < 2) return nullptr;
        const size_t len = static_cast<size_t>(data[0] << 8 | data[1]);
        if (2 + len > size) return nullptr;
        const uint8_t *t = data + 2;
        std::string text;
        if (len >= 2 && ((t[0] == 0xFE && t[1] == 0xFF) || (t[0] == 0xFF && t[1] == 0xFE))) {
            text = utf16_to_utf8(t + 2, len - 2, t[0] == 0xFE);
        } else {
            text = subtitle_utf8(std::string(reinterpret_cast<const char *>(t), len), nullptr);
        }
        cue->text = tidy_lines(text);
    } else {  // WebVTT
        std::string text;
        if (!wvtt_text(data, size, &text)) return nullptr;
        cue->text = tidy_lines(media::webvtt_plain_text(subtitle_utf8(text, "UTF-8")));
    }
    return cue;
}

}  // namespace vplayer
