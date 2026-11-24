// SPDX-License-Identifier: GPL-3.0-or-later
// Resume points and "Continue watching" (history.hpp).
#define WNK_TEST_MAIN
#include "wnk_test.hpp"

#include "history.hpp"

#include <string>
#include <vector>

using namespace vplayer;

namespace {
HistoryEntry entry(const std::string &path, int64_t pos, int64_t len) {
    HistoryEntry e;
    e.path = path;
    e.name = path.substr(path.rfind('/') + 1);
    e.position_ms = pos;
    e.length_ms = len;
    return e;
}
}  // namespace

TEST("watched") {
    CHECK(!is_watched(10'000, 0));
    CHECK(!is_watched(60'000, 600'000));
    CHECK(is_watched(571'000, 600'000));      // last 5 % of 10 min = 30 s
    CHECK(!is_watched(569'000, 600'000));
    CHECK(is_watched(7'080'001, 7'200'000));  // 2 h: last 2 min
    CHECK(!is_watched(6'800'000, 7'200'000));
}

TEST("record_and_resume") {
    std::vector<HistoryEntry> h;
    history_record(&h, entry("/usb/a.mkv", 600'000, 7'200'000));
    history_record(&h, entry("/usb/b.mkv", 20'000, 7'200'000));  // too early to resume
    history_record(&h, entry("smb://nas/c.mkv", 3'000'000, 7'200'000));
    CHECK_EQ(history_resume_ms(h, "/usb/a.mkv"), int64_t{600'000});
    CHECK_EQ(history_resume_ms(h, "/usb/b.mkv"), int64_t{0});
    CHECK_EQ(history_resume_ms(h, "/usb/none.mkv"), int64_t{0});
    CHECK_EQ(h.front().path, std::string("smb://nas/c.mkv"));  // newest first
    history_record(&h, entry("/usb/a.mkv", 900'000, 7'200'000));  // the same file again: moves up
    CHECK_EQ(h.size(), size_t{3});
    CHECK_EQ(h.front().path, std::string("/usb/a.mkv"));
    CHECK_EQ(history_resume_ms(h, "/usb/a.mkv"), int64_t{900'000});
    history_record(&h, entry("/usb/a.mkv", 7'190'000, 7'200'000));  // to the end: watched
    CHECK(h.front().watched);
    CHECK_EQ(history_resume_ms(h, "/usb/a.mkv"), int64_t{0});
}

TEST("continue_list") {
    std::vector<HistoryEntry> h;
    history_record(&h, entry("/a", 600'000, 7'200'000));
    history_record(&h, entry("/b", 7'190'000, 7'200'000));  // watched
    history_record(&h, entry("/c", 5'000, 7'200'000));      // barely started
    history_record(&h, entry("/d", 100'000, 7'200'000));
    const auto c = history_continue(h, 10);
    REQUIRE(c.size() == 2);
    CHECK_EQ(c[0].path, std::string("/d"));
    CHECK_EQ(c[1].path, std::string("/a"));
    CHECK_EQ(history_continue(h, 1).size(), size_t{1});
}

TEST("cap_and_json") {
    std::vector<HistoryEntry> h;
    for (int i = 0; i < 250; ++i) history_record(&h, entry("/f" + std::to_string(i), 60'000, 600'000));
    CHECK_EQ(h.size(), kHistoryMax);
    CHECK_EQ(h.front().path, std::string("/f249"));
    const auto back = history_from_json(history_json(h));
    REQUIRE(back.size() == h.size());
    CHECK_EQ(back[3].path, h[3].path);
    CHECK_EQ(back[3].position_ms, h[3].position_ms);
    CHECK_EQ(back[3].length_ms, h[3].length_ms);
    CHECK(history_from_json("[{\"x\":1}, 5, {\"p\":\"/ok\"}]").size() == 1);
    CHECK(history_from_json("nope").empty());
}
