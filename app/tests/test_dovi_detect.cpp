// SPDX-License-Identifier: GPL-3.0-or-later
// Dolby Vision detection (player/dovi_detect.hpp).
#define WNK_TEST_MAIN
#include "wnk_test.hpp"

#include "player/dovi_detect.hpp"

#include <cstdio>
#include <string>
#include <unistd.h>
#include <vector>

using namespace vplayer;
using Bytes = std::vector<uint8_t>;

namespace {

// Profile 8, level 6, RPU + BL, compatibility 1 (HDR10).
const Bytes kRecord81 = {1, 0, 0x10, 0x35, 0x10, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

Bytes el(Bytes id, const Bytes &payload) {
    if (payload.size() < 127) {
        id.push_back(static_cast<uint8_t>(0x80 | payload.size()));
    } else {
        id.push_back(static_cast<uint8_t>(0x40 | payload.size() >> 8));
        id.push_back(static_cast<uint8_t>(payload.size()));
    }
    id.insert(id.end(), payload.begin(), payload.end());
    return id;
}
Bytes cat(std::initializer_list<Bytes> parts) {
    Bytes out;
    for (const auto &p : parts) out.insert(out.end(), p.begin(), p.end());
    return out;
}
Bytes box(const char *type, const Bytes &payload) {
    const uint32_t n = static_cast<uint32_t>(8 + payload.size());
    Bytes out = {static_cast<uint8_t>(n >> 24), static_cast<uint8_t>(n >> 16), static_cast<uint8_t>(n >> 8),
                 static_cast<uint8_t>(n)};
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

std::string write_temp(const Bytes &b) {
    char name[] = "/tmp/vplayer-dovi-XXXXXX";
    const int fd = mkstemp(name);
    if (fd < 0) return {};
    const bool ok = write(fd, b.data(), b.size()) == static_cast<ssize_t>(b.size());
    close(fd);
    return ok ? std::string(name) : std::string();
}

Bytes mkv(bool with_mapping) {
    Bytes track = cat({el({0xD7}, {1}), el({0x83}, {1})});  // TrackNumber 1, TrackType video
    if (with_mapping)
        track = cat({track, el({0x41, 0xE4}, cat({el({0x41, 0xE7}, {'d', 'v', 'c', 'C'}), el({0x41, 0xED}, kRecord81)}))});
    const Bytes audio = el({0xAE}, cat({el({0xD7}, {2}), el({0x83}, {2})}));
    const Bytes tracks = el({0x16, 0x54, 0xAE, 0x6B}, cat({audio, el({0xAE}, track)}));
    const Bytes info = el({0x15, 0x49, 0xA9, 0x66}, el({0x2A, 0xD7, 0xB1}, {0x0F, 0x42, 0x40}));
    Bytes segment = {0x18, 0x53, 0x80, 0x67, 0x01, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};  // unknown size
    segment = cat({segment, info, tracks, el({0x1F, 0x43, 0xB6, 0x75}, {0xE7, 0x81, 0x00})});
    return cat({el({0x1A, 0x45, 0xDF, 0xA3}, el({0x42, 0x82}, {'w', 'e', 'b', 'm'})), segment});
}

Bytes mp4(bool with_dvcc) {
    Bytes entry(78, 0);
    entry = cat({entry, box("hvcC", Bytes(23, 0))});
    if (with_dvcc) entry = cat({entry, box("dvcC", kRecord81)});
    const Bytes stsd = box("stsd", cat({Bytes{0, 0, 0, 0, 0, 0, 0, 1}, box("hvc1", entry)}));
    const Bytes moov = box("moov", box("trak", box("mdia", box("minf", box("stbl", stsd)))));
    return cat({box("ftyp", {'i', 's', 'o', 'm', 0, 0, 2, 0}), box("mdat", Bytes(100, 0)), moov});
}

Bytes annexb(std::initializer_list<Bytes> nals) {
    Bytes out;
    for (const auto &n : nals) out = cat({out, Bytes{0, 0, 0, 1}, n});
    return out;
}

}  // namespace

TEST("record") {
    DoviConfig c;
    REQUIRE(parse_dovi_record(kRecord81.data(), kRecord81.size(), &c));
    CHECK_EQ(c.profile, 8);
    CHECK_EQ(c.level, 6);
    CHECK(c.rpu && c.bl && !c.el);
    CHECK_EQ(c.bl_compatibility, 1);
}

TEST("matroska_mapping") {
    const std::string with = write_temp(mkv(true)), without = write_temp(mkv(false));
    DoviConfig c;
    CHECK(find_dovi_config(with, &c));
    CHECK_EQ(c.profile, 8);
    DoviConfig none;
    CHECK(!find_dovi_config(without, &none));
    std::remove(with.c_str());
    std::remove(without.c_str());
}

TEST("mp4_dvcc") {
    const std::string with = write_temp(mp4(true)), without = write_temp(mp4(false));
    DoviConfig c;
    CHECK(find_dovi_config(with, &c));
    CHECK_EQ(c.profile, 8);
    CHECK(!find_dovi_config(without, &c));
    CHECK(!find_dovi_config("/nonexistent/file.mkv", &c));
    std::remove(with.c_str());
    std::remove(without.c_str());
}

TEST("nal_scan") {
    const Bytes slice = {0x26, 0x01, 0xAF}, rpu = {0x7C, 0x01, 0x19}, el63 = {0x7E, 0x01, 0x00};
    const Bytes layer1 = {0x02, 0x09, 0x00};  // TRAIL_R, nuh_layer_id 1
    auto a = annexb({slice, rpu});
    CHECK(scan_dovi_nals(a.data(), a.size()).rpu);
    CHECK(!scan_dovi_nals(a.data(), a.size()).el);
    a = annexb({slice, el63, rpu});
    CHECK(scan_dovi_nals(a.data(), a.size()).el);
    a = annexb({slice, layer1});
    CHECK(scan_dovi_nals(a.data(), a.size()).el);
    a = annexb({slice});
    CHECK(!scan_dovi_nals(a.data(), a.size()).rpu);
}

TEST("plans") {
    wnk::media::VideoColour pq, hlg, sdr;
    pq.transfer = 16;
    hlg.transfer = 18;
    sdr.transfer = 2;
    DoviConfig none, rec;
    parse_dovi_record(kRecord81.data(), kRecord81.size(), &rec);
    CHECK_EQ(dovi_plan(rec, {}, sdr, false).profile, 8);  // the record wins
    CHECK_EQ(dovi_plan(none, {true, false}, pq, false).profile, 8);
    CHECK_EQ(dovi_plan(none, {true, false}, hlg, false).profile, 8);
    CHECK_EQ(dovi_plan(none, {true, false}, sdr, false).profile, 5);
    CHECK_EQ(dovi_plan(none, {false, false}, pq, true).profile, -1);
    auto p7 = dovi_plan(none, {true, true}, pq, true);
    CHECK(p7.convert_p7);
    CHECK_EQ(p7.profile, 8);
    auto p7_no_lib = dovi_plan(none, {true, true}, pq, false);
    CHECK(p7_no_lib.convert_p7);
    CHECK_EQ(p7_no_lib.profile, -1);  // base layer only
}
