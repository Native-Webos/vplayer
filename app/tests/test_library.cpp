// SPDX-License-Identifier: GPL-3.0-or-later
// Small library helpers (library.hpp).
#define WNK_TEST_MAIN
#include "wnk_test.hpp"

#include "library.hpp"

using namespace vplayer;

TEST("parse_clock") {
    CHECK_EQ(parse_clock("1:23:45"), int64_t{5'025'000});
    CHECK_EQ(parse_clock("83:10"), int64_t{4'990'000});
    CHECK_EQ(parse_clock("95"), int64_t{95'000});
    CHECK_EQ(parse_clock(" 1.02.03 "), int64_t{3'723'000});
    CHECK_EQ(parse_clock("0:00"), int64_t{0});
    CHECK_EQ(parse_clock("1:75"), int64_t{-1});
    CHECK_EQ(parse_clock("1:2:3:4"), int64_t{-1});
    CHECK_EQ(parse_clock("abc"), int64_t{-1});
    CHECK_EQ(parse_clock(""), int64_t{-1});
}

TEST("clock_text") {
    CHECK_EQ(clock_text(5'025'000), std::string("1:23:45"));
    CHECK_EQ(clock_text(65'000), std::string("1:05"));
    CHECK_EQ(clock_text(-5), std::string("0:00"));
}
