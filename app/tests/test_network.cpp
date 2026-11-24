// SPDX-License-Identifier: GPL-3.0-or-later
// Network helpers and saved places (network.hpp).
#define WNK_TEST_MAIN
#include "wnk_test.hpp"

#include "network.hpp"

#include <string>

using namespace vplayer;
using K = NetEntry::Kind;

TEST("display_names") {
    CHECK_EQ(mrl_display_name("smb://nas/Films%20HD/"), std::string("Films HD"));
    CHECK_EQ(mrl_display_name("smb://nas/"), std::string("nas"));
    CHECK_EQ(mrl_display_name("smb://user@nas"), std::string("nas"));
    CHECK_EQ(mrl_display_name("https://cdn.example.com/live/stream.m3u8?token=1"), std::string("stream.m3u8"));
    CHECK_EQ(mrl_display_name("ftp://host/a/b%C3%A9.mkv"), std::string("b\xC3\xA9.mkv"));
}

TEST("parents") {
    CHECK_EQ(mrl_parent("smb://nas/films/a.mkv"), std::string("smb://nas/films/"));
    CHECK_EQ(mrl_parent("smb://nas/films/"), std::string("smb://nas/"));
    CHECK_EQ(mrl_parent("smb://nas/"), std::string());
    CHECK_EQ(mrl_parent("/local/path"), std::string());
}

TEST("protocols") {
    CHECK_EQ(mrl_protocol("smb://nas/"), std::string("SMB"));
    CHECK_EQ(mrl_protocol("upnp://http://192.168.1.2:8200/desc.xml"), std::string("DLNA"));
    CHECK_EQ(mrl_protocol("https://x/"), std::string("HTTPS"));
    CHECK_EQ(mrl_protocol("/usb/film.mkv"), std::string("FILE"));
}

TEST("urls") {
    CHECK(looks_like_url("https://example.com/a.m3u8"));
    CHECK(looks_like_url("rtsp://10.0.0.2:554/live"));
    CHECK(looks_like_url("smb://nas/films/"));
    CHECK(!looks_like_url("example.com/a.m3u8"));
    CHECK(!looks_like_url("://nothing"));
    CHECK(!looks_like_url("http://"));
    CHECK(!looks_like_url("http://a b"));
}

TEST("kinds") {
    CHECK(net_kind("Films", true) == K::Folder);
    CHECK(net_kind("a.MKV", false) == K::Video);
    CHECK(net_kind("song.flac", false) == K::Audio);
    CHECK(net_kind("a.srt", false) == K::Subtitle);
    CHECK(net_kind("A DLNA title", false) == K::Media);
    CHECK(net_kind("notes.txt", false) == K::Other);
    NetEntry e{"Film", "smb://nas/Film.mkv", K::Video, {}};
    CHECK(e.playable());
    const auto f = to_file_entry(e);
    CHECK_EQ(f.path, std::string("smb://nas/Film.mkv"));
}

TEST("saved_places") {
    SavedPlaces p;
    p.servers.push_back({"NAS", "smb://nas/films/", K::Server, {}});
    for (int i = 0; i < 12; ++i) remember_url(&p, "http://x/" + std::to_string(i));
    remember_url(&p, "http://x/5");  // moves to the front, no duplicate
    CHECK_EQ(p.urls.size(), size_t{10});
    CHECK_EQ(p.urls.front(), std::string("http://x/5"));
    const SavedPlaces back = saved_places_from_json(saved_places_json(p));
    REQUIRE(back.servers.size() == 1);
    CHECK_EQ(back.servers[0].name, std::string("NAS"));
    CHECK_EQ(back.servers[0].mrl, std::string("smb://nas/films/"));
    CHECK(back.servers[0].kind == K::Server);
    CHECK(back.urls == p.urls);
    CHECK(saved_places_from_json("not json").servers.empty());
    CHECK(saved_places_from_json("{\"servers\":[{\"name\":1}],\"urls\":[3]}").servers.empty());
}
