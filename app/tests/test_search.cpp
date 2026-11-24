// SPDX-License-Identifier: GPL-3.0-or-later
// Search matching (search.hpp).
#define WNK_TEST_MAIN
#include "wnk_test.hpp"

#include "search.hpp"

using namespace vplayer;

TEST("fold") {
    CHECK_EQ(search_fold("The.Movie_2024-HDR"), std::string("the movie 2024 hdr"));
    CHECK_EQ(search_fold("Amélie Café Ñandú"), std::string("amelie cafe nandu"));
}

TEST("match") {
    CHECK(search_match("The.Grand.Budapest.Hotel.2014.1080p.mkv", "budapest 2014"));
    CHECK(search_match("Amélie (2001).mkv", "amelie"));
    CHECK(search_match("03 - Café del Mar.flac", "CAFE mar"));
    CHECK(!search_match("Inception.mkv", "inception 2010"));
    CHECK(!search_match("Inception.mkv", ""));
    CHECK(!search_match("Inception.mkv", "   "));
}
