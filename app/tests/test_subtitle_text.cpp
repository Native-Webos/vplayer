// SPDX-License-Identifier: GPL-3.0-or-later
// Text subtitles from libVLC's demuxers → kit cues (player/subtitle_text.hpp).
#define WNK_TEST_MAIN
#include "wnk_test.hpp"

#include "player/subtitle_text.hpp"

#include <string>
#include <vector>

using namespace vplayer;
using wnk::media::kNoTime;
using wnk::media::SubtitleKind;

namespace {

constexpr uint32_t fourcc(const char (&s)[5]) {
    return static_cast<uint32_t>(static_cast<uint8_t>(s[0])) | static_cast<uint32_t>(static_cast<uint8_t>(s[1])) << 8 |
           static_cast<uint32_t>(static_cast<uint8_t>(s[2])) << 16 | static_cast<uint32_t>(static_cast<uint8_t>(s[3])) << 24;
}

std::shared_ptr<const wnk::media::SubtitleCue> cue(const char (&codec)[5], const std::string &data,
                                                   const char *encoding = nullptr, int64_t length = 2'000'000) {
    return text_subtitle_cue(fourcc(codec), reinterpret_cast<const uint8_t *>(data.data()), data.size(), encoding, 3,
                             10'000'000, length);
}

void box(std::string &out, const char *type, const std::string &body) {
    const uint32_t n = static_cast<uint32_t>(8 + body.size());
    out += static_cast<char>(n >> 24);
    out += static_cast<char>(n >> 16);
    out += static_cast<char>(n >> 8);
    out += static_cast<char>(n);
    out.append(type, 4);
    out += body;
}

}  // namespace

TEST("kinds") {
    CHECK(subtitle_kind(fourcc("subt")) == SubtitleKind::Text);
    CHECK(subtitle_kind(fourcc("ssa ")) == SubtitleKind::Text);
    CHECK(subtitle_kind(fourcc("tx3g")) == SubtitleKind::Text);
    CHECK(subtitle_kind(fourcc("wvtt")) == SubtitleKind::Text);
    CHECK(subtitle_kind(fourcc("bdpg")) == SubtitleKind::Bitmap);
    CHECK(subtitle_kind(fourcc("spu ")) == SubtitleKind::Bitmap);
    CHECK(subtitle_kind(fourcc("dvbs")) == SubtitleKind::Bitmap);
    CHECK(subtitle_kind(fourcc("ttxt")) == SubtitleKind::None);
}

TEST("subrip_tags_and_timing") {
    auto c = cue("subt", "<i>Hello</i> <font color=\"#ff0\">there</font>\r\n{\\an8}second line\n\n");
    REQUIRE(c != nullptr);
    CHECK_EQ(c->text, std::string("Hello there\nsecond line"));
    CHECK_EQ(c->track, 3);
    CHECK_EQ(c->start_us, int64_t{10'000'000});
    CHECK_EQ(c->end_us, int64_t{12'000'000});
    CHECK_EQ(c->ass_events.size(), size_t{1});  // {\\an8}: also given to libass
}

TEST("subrip_placement_to_ass") {
    auto c = cue("subt", "{\\an8}<i>Top</i> and <font color=\"#FF8000\">orange</font>\nline");
    REQUIRE(c != nullptr);
    CHECK_EQ(c->text, std::string("Top and orange\nline"));
    REQUIRE(c->ass_events.size() == 1);
    CHECK_EQ(c->ass_events[0], std::string("0,0,Default,,0,0,0,,{\\an8}{\\i1}Top{\\i0} and {\\c&H0080FF&}orange{\\c}\\Nline"));
    CHECK(cue("subt", "<i>no tags</i>")->ass_events.empty());
    CHECK(subrip_ass_header().find("Style: Default,Inter") != std::string::npos);
}

TEST("subrip_open_end_and_nul") {
    auto c = cue("subt", std::string("Line\0", 5), nullptr, 0);
    REQUIRE(c != nullptr);
    CHECK_EQ(c->text, std::string("Line"));
    CHECK_EQ(c->end_us, kNoTime);
}

TEST("subrip_entities_and_ass_breaks") {
    auto c = cue("subt", "Tom &amp; Jerry\\NNext");
    REQUIRE(c != nullptr);
    CHECK_EQ(c->text, std::string("Tom & Jerry\nNext"));
}

TEST("empty_payload_clears") {
    auto c = cue("subt", "");
    REQUIRE(c != nullptr);
    CHECK(c->empty());
}

TEST("windows_1252_fallback") {
    // "café – ok" in Windows-1252: é = E9, en dash = 96.
    auto c = cue("subt", "caf\xE9 \x96 ok");
    REQUIRE(c != nullptr);
    CHECK_EQ(c->text, std::string("caf\xC3\xA9 \xE2\x80\x93 ok"));
    // Valid UTF-8 stays as it is.
    CHECK_EQ(subtitle_utf8("caf\xC3\xA9", nullptr), std::string("caf\xC3\xA9"));
    // A truncated sequence is not UTF-8.
    CHECK_EQ(subtitle_utf8("a\xC3", nullptr), std::string("a\xC3\x83"));
}

TEST("ssa_event") {
    const std::string ev = "12,0,Default,,0,0,0,,{\\i1}Styled{\\i0} text\\Nline two";
    auto c = cue("ssa ", ev);
    REQUIRE(c != nullptr);
    CHECK_EQ(c->text, std::string("Styled text\nline two"));
    REQUIRE(c->ass_events.size() == 1);
    CHECK_EQ(c->ass_events[0], ev);
}

TEST("tx3g") {
    std::string d;
    d += '\0';
    d += '\x05';
    d += "Hi\r\nU";
    d += std::string("\0\0\0\x0cstyl\0\0\0\0", 12);  // a style box after the text
    auto c = cue("tx3g", d);
    REQUIRE(c != nullptr);
    CHECK_EQ(c->text, std::string("Hi\nU"));
    // Length past the end: malformed.
    CHECK(cue("tx3g", std::string("\0\x09" "abc", 5)) == nullptr);
    // UTF-16 with a byte order mark.
    std::string u = std::string("\0\x06\xFE\xFF\0H\0i", 8);
    auto c16 = cue("tx3g", u);
    REQUIRE(c16 != nullptr);
    CHECK_EQ(c16->text, std::string("Hi"));
}

TEST("webvtt_boxes") {
    std::string vttc, sample;
    box(vttc, "sttg", "line:0");
    box(vttc, "payl", "<v Bob>Hello</v> &amp; <i>bye</i>");
    box(sample, "vttc", vttc);
    auto c = cue("wvtt", sample);
    REQUIRE(c != nullptr);
    CHECK_EQ(c->text, std::string("Hello & bye"));

    std::string gap;
    box(gap, "vtte", "");
    auto e = cue("wvtt", gap);
    REQUIRE(e != nullptr);
    CHECK(e->empty());

    CHECK(cue("wvtt", std::string("\0\0\0\x40vttc", 8)) == nullptr);  // box larger than the sample
}

TEST("unknown_codec_and_time") {
    CHECK(cue("pgs ", "x") == nullptr);
    CHECK(text_subtitle_cue(fourcc("subt"), reinterpret_cast<const uint8_t *>("a"), 1, nullptr, 0, kNoTime, 0) == nullptr);
}
