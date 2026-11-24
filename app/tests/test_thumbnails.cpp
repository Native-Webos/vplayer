// SPDX-License-Identifier: GPL-3.0-or-later
// Thumbnail cache names and the BMP writer (thumbnails.hpp).
#define WNK_TEST_MAIN
#include "wnk_test.hpp"

#include "thumbnails.hpp"

#include <cstring>

using namespace vplayer;

TEST("names") {
    const std::string a = thumbnail_name("/usb/a.mkv", 100, 5), b = thumbnail_name("/usb/a.mkv", 101, 5);
    CHECK(a != b);  // a changed file gets a new thumbnail
    CHECK_EQ(a, thumbnail_name("/usb/a.mkv", 100, 5));
    CHECK_EQ(a.size(), size_t{20});
    CHECK(a.compare(16, 4, ".bmp") == 0);
}

TEST("bmp") {
    // 2x2: top row blue, red; bottom row green, white (BGRA).
    const uint8_t px[] = {255, 0, 0, 0, 0, 0, 255, 0, 0, 255, 0, 0, 255, 255, 255, 0};
    const std::string s = bmp_from_bgra(px, 2, 2);
    REQUIRE(s.size() == 54 + 16);
    CHECK(s[0] == 'B' && s[1] == 'M');
    auto u32 = [&](size_t at) {
        uint32_t v;
        std::memcpy(&v, s.data() + at, 4);
        return v;
    };
    CHECK_EQ(u32(10), uint32_t{54});  // pixel data offset
    CHECK_EQ(u32(18), uint32_t{2});   // width
    CHECK_EQ(u32(22), uint32_t{2});   // height (positive: bottom-up)
    // First stored row is the picture's bottom row: green then white, alpha forced opaque.
    CHECK(static_cast<uint8_t>(s[54 + 1]) == 255 && static_cast<uint8_t>(s[54 + 3]) == 255);
    CHECK(static_cast<uint8_t>(s[54 + 4]) == 255 && static_cast<uint8_t>(s[54 + 6]) == 255);
    // Then the top row: blue first.
    CHECK(static_cast<uint8_t>(s[54 + 8]) == 255 && static_cast<uint8_t>(s[54 + 10]) == 0);
}
