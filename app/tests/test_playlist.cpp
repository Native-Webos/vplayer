// SPDX-License-Identifier: GPL-3.0-or-later
// Playlists and IPTV lists (playlist.hpp).
#define WNK_TEST_MAIN
#include "wnk_test.hpp"

#include "playlist.hpp"

#include <string>

using namespace vplayer;

TEST("iptv_m3u") {
    const std::string text =
        "\xEF\xBB\xBF#EXTM3U x-tvg-url=\"http://epg\"\r\n"
        "#EXTINF:-1 tvg-id=\"one\" tvg-logo=\"http://logo/1.png\" group-title=\"News, World\",Channel One HD\r\n"
        "http://iptv.example/live/1.m3u8\r\n"
        "#EXTINF:-1 tvg-name=\"Two\" group-title=\"Sports\",\r\n"
        "#EXTVLCOPT:http-user-agent=Foo\r\n"
        "http://iptv.example/live/2.ts\r\n"
        "\r\n";
    Playlist p;
    std::string err;
    REQUIRE(parse_playlist(text, "http://iptv.example/list.m3u", &p, &err));
    CHECK(!p.is_stream);
    REQUIRE(p.items.size() == 2);
    CHECK_EQ(p.items[0].title, std::string("Channel One HD"));
    CHECK_EQ(p.items[0].group, std::string("News, World"));
    CHECK_EQ(p.items[0].logo, std::string("http://logo/1.png"));
    CHECK_EQ(p.items[0].duration_ms, int64_t{-1});
    CHECK_EQ(p.items[1].title, std::string("Two"));  // empty title: tvg-name
    CHECK_EQ(p.items[1].location, std::string("http://iptv.example/live/2.ts"));
    const auto g = p.groups();
    REQUIRE(g.size() == 2);
    CHECK_EQ(g[1], std::string("Sports"));
}

TEST("local_m3u_relative") {
    const std::string text = "#EXTM3U\n#EXTINF:215,Artist - Song\nSongs/01 Song.mp3\n#EXTGRP:Live\nlive.flac\n";
    Playlist p;
    std::string err;
    REQUIRE(parse_playlist(text, "/usb/Music/mix.m3u", &p, &err));
    REQUIRE(p.items.size() == 2);
    CHECK_EQ(p.items[0].location, std::string("/usb/Music/Songs/01 Song.mp3"));
    CHECK_EQ(p.items[0].duration_ms, int64_t{215000});
    CHECK_EQ(p.items[1].title, std::string("live.flac"));
    CHECK_EQ(p.items[1].group, std::string("Live"));
}

TEST("hls_is_a_stream") {
    Playlist p;
    std::string err;
    REQUIRE(parse_playlist("#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=1\nlow.m3u8\n", "http://x/master.m3u8", &p, &err));
    CHECK(p.is_stream);
    CHECK(p.items.empty());
}

TEST("pls") {
    const std::string text = "[playlist]\nNumberOfEntries=2\nFile2=http://radio/b\nTitle2=Second\nFile1=a.mp3\nLength1=60\n";
    Playlist p;
    std::string err;
    REQUIRE(parse_playlist(text, "/usb/radio.pls", &p, &err));
    REQUIRE(p.items.size() == 2);
    CHECK_EQ(p.items[0].location, std::string("/usb/a.mp3"));
    CHECK_EQ(p.items[0].duration_ms, int64_t{60000});
    CHECK_EQ(p.items[1].title, std::string("Second"));
}

TEST("xspf") {
    const std::string text =
        "<?xml version=\"1.0\"?><playlist version=\"1\" xmlns=\"http://xspf.org/ns/0/\"><trackList>"
        "<track><location>file:///usb/My%20Film.mkv</location><title>My Film</title><duration>5400000</duration></track>"
        "<track><location>clips/b.mp4</location><album>Clips</album><image>http://i/b.jpg</image></track>"
        "</trackList></playlist>";
    Playlist p;
    std::string err;
    REQUIRE(parse_playlist(text, "/usb/list.xspf", &p, &err));
    REQUIRE(p.items.size() == 2);
    CHECK_EQ(p.items[0].location, std::string("/usb/My Film.mkv"));
    CHECK_EQ(p.items[0].duration_ms, int64_t{5400000});
    CHECK_EQ(p.items[1].location, std::string("/usb/clips/b.mp4"));
    CHECK_EQ(p.items[1].title, std::string("b.mp4"));
    CHECK_EQ(p.items[1].group, std::string("Clips"));
}

TEST("saved_lists") {
    const std::vector<SavedPlaylist> l = {{"My TV", "http://p/list.m3u"}, {"", "http://r/radio.pls"}};
    const auto back = saved_playlists_from_json(saved_playlists_json(l));
    REQUIRE(back.size() == 2);
    CHECK_EQ(back[0].name, std::string("My TV"));
    CHECK_EQ(back[1].url, std::string("http://r/radio.pls"));
    CHECK(saved_playlists_from_json("{}").empty());
}

TEST("my_playlists") {
    std::vector<MyPlaylist> l;
    CHECK(my_playlists_add(&l, "Films", {"A", "/usb/a.mkv", {}, {}, -1}));
    CHECK(my_playlists_add(&l, "Films", {"B", "smb://nas/b.mkv", {}, {}, -1}));
    CHECK(!my_playlists_add(&l, "Films", {"A again", "/usb/a.mkv", {}, {}, -1}));  // already there
    CHECK(my_playlists_add(&l, "Music", {"S", "/usb/s.flac", {}, {}, -1}));
    const auto back = my_playlists_from_json(my_playlists_json(l));
    REQUIRE(back.size() == 2);
    REQUIRE(back[0].items.size() == 2);
    CHECK_EQ(back[0].items[1].location, std::string("smb://nas/b.mkv"));
    CHECK_EQ(back[1].name, std::string("Music"));
}

TEST("formats_and_resolution") {
    CHECK(playlist_format("x.M3U8?token=1", "") == PlaylistFormat::M3u);
    CHECK(playlist_format("noext", "[playlist]\n") == PlaylistFormat::Pls);
    CHECK(playlist_format("noext", "hello") == PlaylistFormat::Unknown);
    CHECK_EQ(resolve_location("b.ts", "http://h/a/list.m3u?x=1"), std::string("http://h/a/b.ts"));
    CHECK_EQ(resolve_location("b.ts", "http://h"), std::string("http://h/b.ts"));
    CHECK_EQ(resolve_location("rtsp://x/y", "/usb/l.m3u"), std::string("rtsp://x/y"));
    CHECK_EQ(resolve_location("sub\\c.mkv", "/usb/l.m3u"), std::string("/usb/sub/c.mkv"));
    Playlist p;
    std::string err;
    CHECK(parse_playlist("http://a/1\nhttp://a/2\n", "list", &p, &err));
    CHECK_EQ(p.items.size(), size_t{2});
    CHECK(!parse_playlist("just some text", "notes.txt", &p, &err));
}
