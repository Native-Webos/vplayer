// SPDX-License-Identifier: GPL-3.0-or-later
// Teletext subtitles (player/teletext.hpp).
#define WNK_TEST_MAIN
#include "wnk_test.hpp"

#include "player/teletext.hpp"

#include <string>
#include <vector>

using namespace vplayer;
using Bytes = std::vector<uint8_t>;

namespace {

const uint8_t kHam[16] = {0x15, 0x02, 0x49, 0x5E, 0x64, 0x73, 0x38, 0x2F, 0xD0, 0xC7, 0x8C, 0x9B, 0xA1, 0xB6, 0xFD, 0xEA};

uint8_t odd(uint8_t c) {
    int bits = 0;
    for (int i = 0; i < 7; ++i) bits += c >> i & 1;
    return static_cast<uint8_t>(bits % 2 ? c : c | 0x80);
}

// One EBU data unit (as in the PES: every byte after the field/line byte reversed).
Bytes unit(int mag, int row, const Bytes &payload40) {
    Bytes b = {0xC0, 0xE4};
    const int addr = (mag & 7) | row << 3;
    b.push_back(kHam[addr & 15]);
    b.push_back(kHam[addr >> 4]);
    b.insert(b.end(), payload40.begin(), payload40.end());
    for (size_t k = 1; k < b.size(); ++k) b[k] = TeletextDecoder::reverse(b[k]);
    Bytes out = {0x03, 0x2C};
    out.insert(out.end(), b.begin(), b.end());
    return out;
}

Bytes header(int mag, int page, bool subtitle, bool erase) {
    Bytes p(40, odd(' '));
    p[0] = kHam[page & 15];
    p[1] = kHam[page >> 4];
    p[2] = kHam[0];
    p[3] = kHam[erase ? 8 : 0];
    p[4] = kHam[0];
    p[5] = kHam[subtitle ? 8 : 0];
    p[6] = kHam[0];
    p[7] = kHam[0];
    return unit(mag, 0, p);
}

Bytes text_row(int mag, int row, const std::string &text, bool boxed) {
    Bytes p(40, odd(' '));
    size_t at = 2;
    if (boxed) p[at++] = odd(0x0B), p[at++] = odd(0x0B);
    for (const char c : text) p[at++] = odd(static_cast<uint8_t>(c));
    if (boxed) p[at++] = odd(0x0A), p[at++] = odd(0x0A);
    p[38] = odd('X');  // outside the box: not shown when the row is boxed
    return unit(mag, row, p);
}

Bytes pes(std::initializer_list<Bytes> units) {
    Bytes out = {0x10};
    for (const auto &u : units) out.insert(out.end(), u.begin(), u.end());
    return out;
}

}  // namespace

TEST("hamming_and_reverse") {
    for (int v = 0; v < 16; ++v) CHECK_EQ(TeletextDecoder::unham(kHam[v]), v);
    CHECK_EQ(TeletextDecoder::reverse(0x01), uint8_t{0x80});
    CHECK_EQ(TeletextDecoder::reverse(0xE4), uint8_t{0x27});
}

TEST("named_page") {
    TeletextDecoder dec(8, 0x88);
    const Bytes p = pes({header(8, 0x88, true, true), text_row(8, 20, "Hello there", true),
                         text_row(8, 22, "second line", true)});
    auto cues = dec.decode(p.data(), p.size(), 4, 1'000'000);
    REQUIRE(cues.size() == 1);
    CHECK_EQ(cues[0]->text, std::string("Hello there\nsecond line"));
    CHECK_EQ(cues[0]->start_us, int64_t{1'000'000});
    // Another page of the magazine: its rows are not ours.
    const Bytes other = pes({header(8, 0x10, false, false), text_row(8, 5, "news", false)});
    CHECK(dec.decode(other.data(), other.size(), 4, 2'000'000).empty());
    // The page again with no rows: a clear.
    const Bytes clear = pes({header(8, 0x88, true, true)});
    cues = dec.decode(clear.data(), clear.size(), 4, 3'000'000);
    REQUIRE(cues.size() == 1);
    CHECK(cues[0]->empty());
}

TEST("first_subtitle_page") {
    TeletextDecoder dec(-1, 0);
    const Bytes p = pes({header(1, 0x00, false, false), text_row(1, 3, "index page", false),
                         header(7, 0x77, true, true), text_row(7, 21, "Subtitle", true)});
    auto cues = dec.decode(p.data(), p.size(), 0, 5);
    REQUIRE(cues.size() == 1);
    CHECK_EQ(cues[0]->text, std::string("Subtitle"));
}

TEST("garbage") {
    TeletextDecoder dec(8, 0x88);
    const Bytes junk = {0x10, 0x03, 0x2C, 0x00};
    CHECK(dec.decode(junk.data(), junk.size(), 0, 0).empty());
    const Bytes wrong_id = {0x99, 0x03};
    CHECK(dec.decode(wrong_id.data(), wrong_id.size(), 0, 0).empty());
}
