// SPDX-License-Identifier: GPL-3.0-or-later
// Albums from folders (music.hpp).
#define WNK_TEST_MAIN
#include "wnk_test.hpp"

#include "music.hpp"

using namespace vplayer;
using wnk::platform::FileEntry;
using wnk::platform::FileKind;

namespace {
FileEntry f(const std::string &path, FileKind k) {
    FileEntry e;
    e.path = path;
    e.name = path.substr(path.rfind('/') + 1);
    e.kind = k;
    return e;
}
}  // namespace

TEST("albums") {
    const std::vector<FileEntry> files = {
        f("/usb/Music/Zeta/Best/10 Ten.mp3", FileKind::Audio), f("/usb/Music/Zeta/Best/2 Two.mp3", FileKind::Audio),
        f("/usb/Music/Zeta/Best/Folder.JPG", FileKind::Image), f("/usb/Music/Zeta/Best/back.jpg", FileKind::Image),
        f("/usb/Music/Alpha/Live/a.flac", FileKind::Audio), f("/usb/Music/Alpha/Live/only.png", FileKind::Image),
        f("/usb/loose.ogg", FileKind::Audio)};
    const auto albums = build_albums(files);
    REQUIRE(albums.size() == 3);
    // Artist order: "" (the drive's parent "usb"... ) then Alpha then Zeta.
    const Album *best = nullptr, *live = nullptr;
    for (const auto &a : albums) {
        if (a.title == "Best") best = &a;
        if (a.title == "Live") live = &a;
    }
    REQUIRE(best != nullptr && live != nullptr);
    CHECK_EQ(best->artist, std::string("Zeta"));
    CHECK_EQ(best->cover, std::string("/usb/Music/Zeta/Best/Folder.JPG"));
    REQUIRE(best->tracks.size() == 2);
    CHECK_EQ(best->tracks[0].name, std::string("2 Two.mp3"));  // natural order
    CHECK_EQ(live->cover, std::string("/usb/Music/Alpha/Live/only.png"));  // the only image
}

TEST("covers") {
    CHECK_EQ(pick_cover({"/a/x.jpg", "/a/y.jpg"}), std::string());
    CHECK_EQ(pick_cover({"/a/x.jpg", "/a/AlbumArtSmall.jpg"}), std::string("/a/AlbumArtSmall.jpg"));
    CHECK_EQ(pick_cover({"/a/front.png", "/a/cover.jpg"}), std::string("/a/cover.jpg"));
}

TEST("tag_order") {
    std::vector<TrackTags> t(3);
    t[0].number = 3;
    t[1].number = 1;
    t[2].number = 2;
    CHECK(tag_order(t) == (std::vector<size_t>{1, 2, 0}));
    t[2].number = 0;  // one untagged: the folder's order stays
    CHECK(tag_order(t) == (std::vector<size_t>{0, 1, 2}));
    CHECK(tag_order({}).empty());
    CHECK(read_tags(nullptr, {"/a.mp3"}).size() == 1);
}
