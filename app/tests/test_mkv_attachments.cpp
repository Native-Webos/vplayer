// SPDX-License-Identifier: GPL-3.0-or-later
// Fonts attached to Matroska files (player/mkv_attachments.hpp).
#define WNK_TEST_MAIN
#include "wnk_test.hpp"

#include "player/mkv_attachments.hpp"

#include <cstdio>
#include <string>
#include <unistd.h>
#include <vector>

using namespace vplayer;
using Bytes = std::vector<uint8_t>;

namespace {

Bytes el(Bytes id, const Bytes &payload) {
    // 8-byte sizes everywhere: the SeekHead's positions do not depend on payload lengths.
    id.push_back(0x01);
    for (int i = 6; i >= 0; --i) id.push_back(static_cast<uint8_t>(payload.size() >> (8 * i)));
    id.insert(id.end(), payload.begin(), payload.end());
    return id;
}
Bytes cat(std::initializer_list<Bytes> parts) {
    Bytes out;
    for (const auto &p : parts) out.insert(out.end(), p.begin(), p.end());
    return out;
}
Bytes str(const std::string &s) { return Bytes(s.begin(), s.end()); }

Bytes attached(const std::string &name, const std::string &mime, const Bytes &data) {
    return el({0x61, 0xA7}, cat({el({0x46, 0x6E}, str(name)), el({0x46, 0x60}, str(mime)), el({0x46, 0x5C}, data)}));
}

Bytes attachments() {
    return el({0x19, 0x41, 0xA4, 0x69}, cat({attached("Fancy.ttf", "application/x-truetype-font", Bytes(100, 7)),
                                             attached("cover.jpg", "image/jpeg", Bytes(50, 1)),
                                             attached("Other.OTF", "application/octet-stream", Bytes(30, 9))}));
}

// Attachments either among the first children, or after a cluster with a SeekHead pointing at them.
Bytes mkv(bool at_end) {
    const Bytes header = el({0x1A, 0x45, 0xDF, 0xA3}, el({0x42, 0x82}, str("matroska")));
    const Bytes tracks = el({0x16, 0x54, 0xAE, 0x6B}, el({0xAE}, el({0x83}, {1})));
    const Bytes cluster = el({0x1F, 0x43, 0xB6, 0x75}, el({0xE7}, {0}));
    if (!at_end) return cat({header, el({0x18, 0x53, 0x80, 0x67}, cat({tracks, attachments(), cluster}))});
    // SeekHead size is fixed (8-byte sizes), so the position can be computed up front.
    auto seek_head = [](uint64_t pos) {
        Bytes p;
        for (int i = 7; i >= 0; --i) p.push_back(static_cast<uint8_t>(pos >> (8 * i)));
        return el({0x11, 0x4D, 0x9B, 0x74},
                  el({0x4D, 0xBB}, cat({el({0x53, 0xAB}, {0x19, 0x41, 0xA4, 0x69}), el({0x53, 0xAC}, p)})));
    };
    const size_t before = seek_head(0).size() + tracks.size() + cluster.size();
    return cat({header, el({0x18, 0x53, 0x80, 0x67}, cat({seek_head(before), tracks, cluster, attachments()}))});
}

std::string write_temp(const Bytes &b) {
    char name[] = "/tmp/vplayer-att-XXXXXX";
    const int fd = mkstemp(name);
    if (fd < 0) return {};
    const bool ok = write(fd, b.data(), b.size()) == static_cast<ssize_t>(b.size());
    close(fd);
    return ok ? std::string(name) : std::string();
}

}  // namespace

TEST("font_types") {
    CHECK(is_font_attachment("a.ttf", ""));
    CHECK(is_font_attachment("A.OTF", ""));
    CHECK(is_font_attachment("x", "font/woff2"));
    CHECK(is_font_attachment("x", "application/vnd.ms-opentype"));
    CHECK(!is_font_attachment("cover.jpg", "image/jpeg"));
}

TEST("attachments_in_front") {
    const std::string path = write_temp(mkv(false));
    const auto fonts = mkv_fonts(path);
    REQUIRE(fonts.size() == 2);
    CHECK_EQ(fonts[0].name, std::string("Fancy.ttf"));
    CHECK_EQ(fonts[0].data->size(), size_t{100});
    CHECK_EQ(fonts[1].name, std::string("Other.OTF"));
    std::remove(path.c_str());
}

TEST("attachments_through_seek_head") {
    const std::string path = write_temp(mkv(true));
    const auto fonts = mkv_fonts(path);
    CHECK_EQ(fonts.size(), size_t{2});
    std::remove(path.c_str());
}

TEST("not_matroska") {
    const std::string path = write_temp(Bytes{0, 0, 0, 0x18, 'f', 't', 'y', 'p'});
    CHECK(mkv_fonts(path).empty());
    CHECK(mkv_fonts("/nonexistent").empty());
    std::remove(path.c_str());
}
