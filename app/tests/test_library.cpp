// SPDX-License-Identifier: GPL-3.0-or-later
// Small library helpers (library.hpp).
#define WNK_TEST_MAIN
#include "wnk_test.hpp"

#include "library.hpp"
#include "player/playback.hpp"

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

TEST("disc folders and images") {
    using wnk::platform::FileEntry;
    using wnk::platform::FileKind;
    FileEntry ts;
    ts.name = "VIDEO_TS";
    ts.kind = FileKind::Folder;
    FileEntry other;
    other.name = "Extras";
    other.kind = FileKind::Folder;
    CHECK_EQ(vplayer::disc_folder_mrl("/usb/Film", {other, ts}), std::string("dvd:///usb/Film"));
    FileEntry bd;
    bd.name = "bdmv";
    bd.kind = FileKind::Folder;
    CHECK_EQ(vplayer::disc_folder_mrl("/usb/Film", {bd}), std::string("bluray:///usb/Film"));
    FileEntry file;
    file.name = "VIDEO_TS";  // a file of that name is not a disc
    file.kind = FileKind::Other;
    CHECK(vplayer::disc_folder_mrl("/usb/Film", {file, other}).empty());
    CHECK_EQ(vplayer::disc_folder_kind("dvd:///usb/Film"), std::string("DVD"));
    CHECK_EQ(vplayer::disc_folder_kind("bluray:///usb/Film"), std::string("BD"));
    CHECK(vplayer::disc_folder_kind("/usb/Film.iso").empty());
    FileEntry iso;
    iso.name = "Film.iso";
    iso.kind = FileKind::Other;
    CHECK(vplayer::is_disc(iso));
    FileEntry folder;
    folder.name = "Film";
    folder.path = "dvd:///usb/Film";
    folder.kind = FileKind::Video;
    CHECK(vplayer::is_disc(folder));
    CHECK(!vplayer::is_disc(other));
}

TEST("audio effects: the filters they need") {
    vplayer::AudioEffects fx;
    CHECK(fx.filter_chain().empty());
    CHECK(!fx.active());
    fx.compressor = true;
    fx.pitch = -3;
    CHECK_EQ(fx.filter_chain(), std::string("compressor:scaletempo_pitch"));
    fx.normalize = fx.spatializer = true;
    CHECK_EQ(fx.filter_chain(), std::string("normvol:compressor:spatializer:scaletempo_pitch"));
    CHECK(fx.active());
    vplayer::AudioEffects other = fx;
    other.pitch = 2;
    CHECK(other != fx);
}
