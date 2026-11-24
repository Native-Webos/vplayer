// SPDX-License-Identifier: GPL-3.0-or-later
// CEA-708 closed captions (player/cea708.hpp).
#define WNK_TEST_MAIN
#include "wnk_test.hpp"

#include "player/cea708.hpp"

#include <string>
#include <vector>

using namespace vplayer;
using Bytes = std::vector<uint8_t>;

namespace {

// A DTVCC packet carrying `service1` as one service-1 block, as cc_data pairs.
std::vector<Cc708Pair> packet(const Bytes &service1, int seq = 0) {
    Bytes p = {0};
    p.push_back(static_cast<uint8_t>(1 << 5 | service1.size()));
    p.insert(p.end(), service1.begin(), service1.end());
    if (p.size() % 2) p.push_back(0);
    p[0] = static_cast<uint8_t>((seq & 3) << 6 | (p.size() / 2));
    std::vector<Cc708Pair> pairs;
    for (size_t i = 0; i < p.size(); i += 2) pairs.push_back({i == 0, p[i], p[i + 1]});
    return pairs;
}

// DefineWindow `id`: rows, cols, visible.
Bytes define(int id, int rows, int cols, bool visible, int priority = 0) {
    return {static_cast<uint8_t>(0x98 + id), static_cast<uint8_t>((visible ? 0x20 : 0) | priority), 0, 0,
            static_cast<uint8_t>(rows - 1), static_cast<uint8_t>(cols - 1), 0};
}

Bytes text(const std::string &s) { return Bytes(s.begin(), s.end()); }

Bytes cat(std::initializer_list<Bytes> parts) {
    Bytes out;
    for (const auto &p : parts) out.insert(out.end(), p.begin(), p.end());
    return out;
}

struct Feeder {
    Cea708Decoder dec{9};
    std::vector<Cea708Decoder::Cue> cues;
    int64_t t = 0;
    void send(const Bytes &service1) {
        t += 33'366;
        for (const auto &pair : packet(service1))
            for (auto &c : dec.feed(pair, t)) cues.push_back(c);
    }
    void later(int64_t us) {
        t += us;
        for (auto &c : dec.tick(t)) cues.push_back(c);
    }
};

}  // namespace

TEST("hidden_window_then_shown") {
    Feeder f;
    f.send(cat({define(0, 2, 32, false), text("HELLO"), {0x0D}, text("WORLD")}));
    const size_t before = f.cues.size();
    f.send({0x89, 0x01});  // DSW window 0
    REQUIRE(f.cues.size() == before + 1);
    CHECK_EQ(f.cues.back()->text, std::string("HELLO\nWORLD"));
    f.send({0x8A, 0x01});  // HDW
    CHECK(f.cues.back()->empty());
    CHECK(f.dec.seen());
}

TEST("typed_text_throttled_then_shown") {
    Feeder f;
    f.send(define(1, 1, 32, true));  // shown at once (empty)
    f.send(text("ABC"));              // within 250 ms of the last cue: held
    CHECK(f.cues.empty() || f.cues.back()->text.empty());
    f.later(300'000);
    REQUIRE(!f.cues.empty());
    CHECK_EQ(f.cues.back()->text, std::string("ABC"));
}

TEST("characters") {
    Feeder f;
    f.send(cat({define(0, 1, 32, true), {'c', 'a', 'f', 0xE9, ' ', 0x7F, ' ', 0x10, 0x25}}));
    f.later(300'000);
    REQUIRE(!f.cues.empty());
    CHECK_EQ(f.cues.back()->text, std::string("caf\xC3\xA9 \xE2\x99\xAA \xE2\x80\xA6"));
}

TEST("carriage_return_scrolls") {
    Feeder f;
    f.send(cat({define(0, 2, 32, true), text("ONE"), {0x0D}, text("TWO"), {0x0D}, text("THREE")}));
    f.later(300'000);
    CHECK_EQ(f.cues.back()->text, std::string("TWO\nTHREE"));
}

TEST("pen_location_backspace_and_clear") {
    Feeder f;
    f.send(cat({define(0, 3, 32, true), {0x92, 2, 4}, text("XY"), {0x08}, text("Z")}));
    f.later(300'000);
    CHECK_EQ(f.cues.back()->text, std::string("XZ"));
    f.send({0x88, 0x01});  // CLW
    CHECK(f.cues.back()->empty());
}

TEST("other_services_ignored") {
    Feeder f;
    Bytes p = {0, static_cast<uint8_t>(2 << 5 | 3), 0x98, 0x20, 0};  // service 2, cut short
    p[0] = static_cast<uint8_t>(p.size() / 2 + p.size() % 2);
    if (p.size() % 2) p.push_back(0);
    for (size_t i = 0; i < p.size(); i += 2)
        for (auto &c : f.dec.feed({i == 0, p[i], p[i + 1]}, 1000)) f.cues.push_back(c);
    CHECK(f.cues.empty());
    CHECK(!f.dec.seen());
}

TEST("sei_extraction_708") {
    // GA94 cc_data: a 608 pair (type 0), a 708 start (type 3) and continuation (type 2).
    Bytes payload = {0xB5, 0x00, 0x31, 'G', 'A', '9', '4', 0x03, 0x40 | 3, 0xFF,
                     0xFC, 0x94, 0x20,  // 608 field 1
                     0xFF, 0x02, 0x21,  // 708 packet start
                     0xFE, 0x41, 0x42}; // 708 continuation
    Bytes sei = {0x06, 0x04, static_cast<uint8_t>(payload.size())};
    sei.insert(sei.end(), payload.begin(), payload.end());
    sei.push_back(0x80);
    Bytes au = {0, 0, 0, 1};
    au.insert(au.end(), sei.begin(), sei.end());
    const auto pairs = cc708_pairs(au.data(), au.size(), false);
    REQUIRE(pairs.size() == 2);
    CHECK(pairs[0].start);
    CHECK(pairs[0].b1 == 0x02 && pairs[0].b2 == 0x21);
    CHECK(!pairs[1].start);
    CHECK(pairs[1].b1 == 0x41 && pairs[1].b2 == 0x42);
}
