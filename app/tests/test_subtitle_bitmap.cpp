// SPDX-License-Identifier: GPL-3.0-or-later
// Bitmap subtitles (PGS, DVD) → kit cues (player/subtitle_bitmap.hpp).
#define WNK_TEST_MAIN
#include "wnk_test.hpp"

#include "player/subtitle_bitmap.hpp"

#include <vector>

using namespace vplayer;
using wnk::media::kNoTime;

namespace {

constexpr uint32_t fourcc(const char (&s)[5]) {
    return static_cast<uint32_t>(static_cast<uint8_t>(s[0])) | static_cast<uint32_t>(static_cast<uint8_t>(s[1])) << 8 |
           static_cast<uint32_t>(static_cast<uint8_t>(s[2])) << 16 | static_cast<uint32_t>(static_cast<uint8_t>(s[3])) << 24;
}

void seg(std::vector<uint8_t> &out, uint8_t type, const std::vector<uint8_t> &body) {
    out.push_back(type);
    out.push_back(static_cast<uint8_t>(body.size() >> 8));
    out.push_back(static_cast<uint8_t>(body.size()));
    out.insert(out.end(), body.begin(), body.end());
}

// A PGS display set: 1920x1080, `objects` placements of object 0 at (100, 900).
std::vector<uint8_t> pgs_set(int objects, bool epoch) {
    std::vector<uint8_t> d;
    std::vector<uint8_t> pcs = {0x07, 0x80, 0x04, 0x38, 0x10, 0x00, 0x01, static_cast<uint8_t>(epoch ? 0x80 : 0x00),
                                0x00, 0x00, static_cast<uint8_t>(objects)};
    if (objects) pcs.insert(pcs.end(), {0x00, 0x00, 0x00, 0x00, 0x00, 100, 0x03, 0x84});  // obj 0, x 100, y 900
    seg(d, 0x16, pcs);
    if (epoch) {
        seg(d, 0x14, {0x00, 0x00, 0x01, 235, 128, 128, 255, 0x02, 16, 128, 128, 128});  // 1 white, 2 half black
        // Object 0: 3x2. Line 1: white, white, (run of 1 colour 2). Line 2: 3 white (0x00 0x03 run form) .
        std::vector<uint8_t> rle = {0x01, 0x01, 0x00, 0x81, 0x02, 0x00, 0x00, 0x00, 0x83, 0x01, 0x00, 0x00};
        std::vector<uint8_t> ods = {0x00, 0x00, 0x00, 0xC0};
        const size_t len = rle.size() + 4;
        ods.insert(ods.end(), {static_cast<uint8_t>(len >> 16), static_cast<uint8_t>(len >> 8), static_cast<uint8_t>(len),
                               0x00, 0x03, 0x00, 0x02});
        ods.insert(ods.end(), rle.begin(), rle.end());
        seg(d, 0x15, ods);
    }
    seg(d, 0x80, {});
    return d;
}

uint8_t px(const wnk::media::SubtitleBitmap &b, int x, int y, int c) {
    return b.rgba[(static_cast<size_t>(y) * b.width + x) * 4 + c];
}

}  // namespace

TEST("pgs_display_set") {
    BitmapSubtitleDecoder dec(fourcc("bdpg"), nullptr, 0, 0);
    const auto d = pgs_set(1, true);
    auto cues = dec.decode(d.data(), d.size(), 4, 5'000'000);
    REQUIRE(cues.size() == 1);
    const auto &c = *cues[0];
    CHECK_EQ(c.track, 4);
    CHECK_EQ(c.start_us, int64_t{5'000'000});
    CHECK_EQ(c.end_us, kNoTime);
    CHECK_EQ(c.canvas_w, 1920);
    CHECK_EQ(c.canvas_h, 1080);
    REQUIRE(c.bitmaps.size() == 1);
    const auto &b = c.bitmaps[0];
    CHECK_EQ(b.x, 100);
    CHECK_EQ(b.y, 900);
    CHECK_EQ(b.width, 3);
    CHECK_EQ(b.height, 2);
    CHECK(px(b, 0, 0, 0) >= 250 && px(b, 0, 0, 3) == 255);  // white, opaque
    CHECK(px(b, 2, 0, 0) <= 5 && px(b, 2, 0, 3) == 128);    // black, half transparent
    CHECK(px(b, 2, 1, 0) >= 250);                           // second line: three white
}

TEST("pgs_clear_and_reuse") {
    BitmapSubtitleDecoder dec(fourcc("bdpg"), nullptr, 0, 0);
    auto d = pgs_set(1, true);
    dec.decode(d.data(), d.size(), 0, 1'000'000);
    d = pgs_set(0, false);  // composition with no object: clears
    auto cues = dec.decode(d.data(), d.size(), 0, 3'000'000);
    REQUIRE(cues.size() == 1);
    CHECK(cues[0]->empty());
    d = pgs_set(1, false);  // the epoch's object and palette are reused
    cues = dec.decode(d.data(), d.size(), 0, 4'000'000);
    REQUIRE(cues.size() == 1);
    CHECK_EQ(cues[0]->bitmaps.size(), size_t{1});
}

TEST("pgs_fragments_without_end") {
    BitmapSubtitleDecoder dec(fourcc("bdpg"), nullptr, 0, 0);
    auto d = pgs_set(1, true);
    d.resize(d.size() - 3);  // no END segment yet: nothing to show
    CHECK(dec.decode(d.data(), d.size(), 0, 1'000'000).empty());
    const uint8_t end[] = {0x80, 0x00, 0x00};
    CHECK_EQ(dec.decode(end, sizeof end, 0, 1'000'000).size(), size_t{1});
}

TEST("dvd_subpicture") {
    // 4x2 at (10, 20), each line a run of 4 pixels of colour 1; shown from 0, stopped at 25 ticks.
    std::vector<uint8_t> d = {0, 0, 0, 0, 0x11, 0x11};
    const size_t seq1 = d.size(), seq2 = seq1 + 4 + 3 + 3 + 7 + 5 + 1 + 1;
    d.insert(d.end(), {0x00, 0x00, static_cast<uint8_t>(seq2 >> 8), static_cast<uint8_t>(seq2)});
    d.insert(d.end(), {0x03, 0x00, 0x10});              // colour index 1 → palette 1, 0 → palette 0
    d.insert(d.end(), {0x04, 0x00, 0xF0});              // alpha: colour 1 opaque, 0 transparent
    d.insert(d.end(), {0x05, 0x00, 0xA0, 13, 0x01, 0x40, 21});
    d.insert(d.end(), {0x06, 0x00, 0x04, 0x00, 0x05});
    d.insert(d.end(), {0x01, 0xFF});
    REQUIRE(d.size() == seq2);
    d.insert(d.end(), {0x00, 25, static_cast<uint8_t>(seq2 >> 8), static_cast<uint8_t>(seq2), 0x02, 0xFF});
    d[0] = static_cast<uint8_t>(d.size() >> 8);
    d[1] = static_cast<uint8_t>(d.size());
    d[2] = static_cast<uint8_t>(seq1 >> 8);
    d[3] = static_cast<uint8_t>(seq1);

    uint32_t palette[16] = {};
    palette[1] = 0x00EB8080;  // white (Y 235, Cb 128, Cr 128)
    BitmapSubtitleDecoder dec(fourcc("spu "), palette, 720, 576);
    auto cues = dec.decode(d.data(), d.size(), 7, 2'000'000);
    REQUIRE(cues.size() == 1);
    const auto &c = *cues[0];
    CHECK_EQ(c.start_us, int64_t{2'000'000});
    CHECK_EQ(c.end_us, int64_t{2'000'000 + 25 * 1024 * 1000000LL / 90000});
    CHECK_EQ(c.canvas_w, 720);
    REQUIRE(c.bitmaps.size() == 1);
    const auto &b = c.bitmaps[0];
    CHECK_EQ(b.x, 10);
    CHECK_EQ(b.y, 20);
    CHECK_EQ(b.width, 4);
    CHECK_EQ(b.height, 2);
    CHECK(px(b, 3, 1, 1) >= 250 && px(b, 3, 1, 3) == 255);
}

TEST("dvd_no_stop") {
    // Start only, then a stop dated 0xFFFF: the cue lasts until the next subpicture.
    std::vector<uint8_t> d = {0, 0, 0, 0, 0x11, 0x11};
    const size_t seq1 = d.size(), seq2 = seq1 + 4 + 7 + 5 + 1 + 1;
    d.insert(d.end(), {0x00, 0x00, static_cast<uint8_t>(seq2 >> 8), static_cast<uint8_t>(seq2)});
    d.insert(d.end(), {0x05, 0x00, 0xA0, 13, 0x01, 0x40, 21});
    d.insert(d.end(), {0x06, 0x00, 0x04, 0x00, 0x05});
    d.insert(d.end(), {0x01, 0xFF});
    d.insert(d.end(), {0xFF, 0xFF, static_cast<uint8_t>(seq2 >> 8), static_cast<uint8_t>(seq2), 0x02, 0xFF});
    d[1] = static_cast<uint8_t>(d.size());
    d[3] = static_cast<uint8_t>(seq1);
    BitmapSubtitleDecoder dec(fourcc("spu "), nullptr, 0, 0);
    auto cues = dec.decode(d.data(), d.size(), 0, 1'000'000);
    REQUIRE(cues.size() == 1);
    CHECK_EQ(cues[0]->end_us, kNoTime);
    CHECK_EQ(cues[0]->canvas_w, 720);  // nothing known: PAL DVD
}

TEST("malformed_is_safe") {
    BitmapSubtitleDecoder pgs(fourcc("bdpg"), nullptr, 0, 0);
    const uint8_t junk[] = {0x16, 0xFF, 0xFF, 0x01};
    CHECK(pgs.decode(junk, sizeof junk, 0, 0).empty());
    BitmapSubtitleDecoder dvd(fourcc("spu "), nullptr, 0, 0);
    const uint8_t short_spu[] = {0x00, 0x08, 0x00, 0x40, 0x01};
    CHECK(dvd.decode(short_spu, sizeof short_spu, 0, 0).size() <= 1);
}
