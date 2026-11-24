// SPDX-License-Identifier: GPL-3.0-or-later
//
// Playlists and IPTV channel lists, read by the app (not libVLC) so their groups and logos can be
// shown:
//
//   M3U / M3U8   #EXTINF:<duration> tvg-logo="..." group-title="...",<title>, then the address;
//                #EXTGRP:<group> also names the group. An HLS playlist (#EXT-X-TARGETDURATION,
//                #EXT-X-STREAM-INF...) is a stream, not a list: it is reported as such and played.
//   PLS          [playlist] FileN= / TitleN= / LengthN=
//   XSPF         <track><location>, <title>, <image>, <album> (as the group), <duration> (ms)
//
// Relative entries are resolved against the playlist's own location (a folder or a URL).
// Pure, tested on the host.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace vplayer {

struct PlaylistItem {
    std::string title;
    std::string location;  // a local path or an address libVLC opens
    std::string group;     // IPTV group-title, XSPF album; empty when none
    std::string logo;      // an image address, when given
    int64_t duration_ms = -1;  // -1 unknown or live
};

struct Playlist {
    std::vector<PlaylistItem> items;
    bool is_stream = false;  // HLS: play the playlist itself
    std::vector<std::string> groups() const;  // in first-seen order
};

enum class PlaylistFormat : uint8_t { Unknown, M3u, Pls, Xspf };
// From the name's extension, else the content's first characters.
PlaylistFormat playlist_format(const std::string &name, const std::string &text);
// False with a reason when the text is not a playlist this reads.
bool parse_playlist(const std::string &text, const std::string &location, Playlist *out, std::string *error);
// An entry's location resolved against the playlist's location (its folder or URL).
std::string resolve_location(const std::string &entry, const std::string &playlist_location);

// Lists the viewer added by address (IPTV providers, radio lists), kept in the app's store.
struct SavedPlaylist {
    std::string name;
    std::string url;
};
std::string saved_playlists_json(const std::vector<SavedPlaylist> &lists);
std::vector<SavedPlaylist> saved_playlists_from_json(const std::string &json);

// The viewer's own playlists (made in the app), kept in its store.
struct MyPlaylist {
    std::string name;
    std::vector<PlaylistItem> items;
};
std::string my_playlists_json(const std::vector<MyPlaylist> &lists);
std::vector<MyPlaylist> my_playlists_from_json(const std::string &json);
// Adds `item` to the list called `name` (made when missing); false when it is there already.
bool my_playlists_add(std::vector<MyPlaylist> *lists, const std::string &name, const PlaylistItem &item);

}  // namespace vplayer
