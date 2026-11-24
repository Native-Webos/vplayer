// SPDX-License-Identifier: GPL-3.0-or-later
// CEA-608 closed captions (player/cea608.hpp).
#define WNK_TEST_MAIN
#include "wnk_test.hpp"

#include "player/cea608.hpp"

#include <string>
#include <vector>

using namespace vplayer;
using Bytes = std::vector<uint8_t>;

namespace {

uint8_t odd(uint8_t c) {
    int bits = 0;
    for (int i = 0; i < 7; ++i) bits += c >> i & 1;
    return static_cast<uint8_t>(bits % 2 ? c : c | 0x80);
}

// Feeds control codes twice (as broadcast) and text pairs once; returns the cues made.
struct Feeder {
    Cea608Decoder dec{9};
    std::vector<Cea608Decoder::Cue> cues;
    int64_t t = 0;
    void pair(uint8_t a, uint8_t b) {
        t += 33'366;
        for (auto &c : dec.feed(odd(a), odd(b), t)) cues.push_back(c);
    }
    void control(uint8_t a, uint8_t b) {
        pair(a, b);
        pair(a, b);
    }
    void text(const std::string &s) {
        for (size_t i = 0; i < s.size(); i += 2) pair(static_cast<uint8_t>(s[i]), i + 1 < s.size() ? static_cast<uint8_t>(s[i + 1]) : 0);
    }
};

}  // namespace

TEST("pop_on") {
    Feeder f;
    f.control(0x14, 0x20);  // resume caption loading
    f.control(0x14, 0x70);  // PAC: row 15, indent 0
    f.text("HELLO");
    f.control(0x13, 0x50);  // PAC: row 14 (row index 13)
    f.text("THERE");
    CHECK(f.cues.empty());  // loaded off screen
    f.control(0x14, 0x2F);  // end of caption
    REQUIRE(f.cues.size() == 1);
    CHECK_EQ(f.cues[0]->text, std::string("THERE\nHELLO"));
    f.control(0x14, 0x2C);  // erase displayed memory
    REQUIRE(f.cues.size() == 2);
    CHECK(f.cues[1]->empty());
}

TEST("roll_up") {
    Feeder f;
    f.control(0x14, 0x25);  // roll-up, 2 rows
    f.control(0x14, 0x70);
    f.text("FIRST");
    f.control(0x14, 0x2D);  // carriage return
    f.text("SECOND");
    f.pair(0, 0);
    f.t += 400'000;
    f.text("  ");
    REQUIRE(!f.cues.empty());
    CHECK_EQ(f.cues.back()->text, std::string("FIRST\nSECOND"));
}

TEST("roll_up_then_padding") {
    // Text painted quickly, then only padding: it shows once the 300 ms are up.
    Feeder f;
    f.control(0x14, 0x25);
    f.control(0x14, 0x70);
    f.text("LIVE");
    for (int i = 0; i < 15; ++i) f.pair(0, 0);
    REQUIRE(!f.cues.empty());
    CHECK_EQ(f.cues.back()->text, std::string("LIVE"));
}

TEST("characters") {
    Feeder f;
    f.control(0x14, 0x29);  // paint-on
    f.control(0x14, 0x70);
    f.text("caf");
    f.control(0x11, 0x3A);  // special: è... (0x3A)
    f.text("A*");           // 0x2A is á
    f.control(0x12, 0x22);  // extended Ó replaces the previous character
    f.t += 400'000;
    f.pair(0x20, 0x20);
    REQUIRE(!f.cues.empty());
    CHECK_EQ(f.cues.back()->text, std::string("caf\xC3\xA8" "A\xC3\x93"));
}

TEST("channel_2_ignored") {
    Feeder f;
    f.control(0x1C, 0x20);  // CC2 resume caption loading
    f.control(0x1C, 0x2F);  // CC2 end of caption
    CHECK(f.cues.empty());
}

TEST("sei_extraction") {
    // H.264: AUD, then an SEI with a GA94 cc_data payload carrying two field-1 pairs and one field-2.
    Bytes payload = {0xB5, 0x00, 0x31, 'G', 'A', '9', '4', 0x03, 0x40 | 3, 0xFF,
                     0xFC, 0x94, 0x20,   // field 1: valid, type 0
                     0xFD, 0x94, 0x20,   // field 2: not CC1
                     0xFC, 0xC8, 0xC5};  // field 1: "HE" with parity
    Bytes sei = {0x06, 0x04, static_cast<uint8_t>(payload.size())};
    sei.insert(sei.end(), payload.begin(), payload.end());
    sei.push_back(0x80);
    Bytes au = {0, 0, 0, 1, 0x09, 0xF0, 0, 0, 0, 1};
    au.insert(au.end(), sei.begin(), sei.end());
    au.insert(au.end(), {0, 0, 1, 0x65, 0x88, 0x00, 0x00, 0x03, 0x01});  // a slice with emulation prevention
    const auto pairs = cc608_pairs(au.data(), au.size(), false);
    REQUIRE(pairs.size() == 2);
    CHECK(pairs[0] == (CcPair{0x94, 0x20}));
    CHECK(pairs[1] == (CcPair{0xC8, 0xC5}));
    CHECK(cc608_pairs(au.data(), au.size(), true).empty());  // not HEVC SEI types
}
