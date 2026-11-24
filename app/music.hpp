// SPDX-License-Identifier: GPL-3.0-or-later
//
// Music from the drives, grouped the way people file it: a folder of audio files is an album, its
// parent folder the artist, and a cover image in it (cover/folder/front/albumart*.jpg|png, else the
// folder's only image) its artwork. Tags are not read (that would mean opening every file).
// Pure, tested on the host.
#pragma once

#include "wnk/platform/files.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

struct libvlc_instance_t;
#include <vector>

namespace vplayer {

struct Album {
    std::string title;   // the folder's name
    std::string artist;  // the parent folder's name; empty at a drive's root
    std::string folder;
    std::string cover;   // an image path, or empty
    std::vector<wnk::platform::FileEntry> tracks;  // natural order
};

// The artists of `albums` (their folder's parent), natural order, each with its albums' indexes in
// the albums' order; albums at a drive's root have no artist and are left out.
struct Artist {
    std::string name;
    std::vector<size_t> albums;
};
std::vector<Artist> group_artists(const std::vector<Album> &albums);

// What a track's tags say (libVLC reads them; empty fields when it found none).
struct TrackTags {
    std::string title, artist, album;
    int number = 0;
    int64_t duration_ms = 0;
};
// A reference of its own to the libVLC instance (libvlc_retain), so work on a worker outlives the
// app's (which releases it when stopping). Take it on the main thread. Null for null.
std::shared_ptr<libvlc_instance_t> hold_vlc(libvlc_instance_t *vlc);
// Reads the tags of `paths` with libVLC (local files, ~2 s at most each). Blocking: a worker; stops
// early (empty tags after) when `stop` returns true. Builds without libVLC give empty tags.
std::vector<TrackTags> read_tags(libvlc_instance_t *vlc, const std::vector<std::string> &paths,
                                 const std::function<bool()> &stop = {});
// The tracks in tag order when every track has a number, else as they are (indexes into tags).
std::vector<size_t> tag_order(const std::vector<TrackTags> &tags);

// `files`: audio files and images from scan_files. Albums in natural order of artist, then title.
std::vector<Album> build_albums(const std::vector<wnk::platform::FileEntry> &files);
// The cover among a folder's image names (lower-case comparisons); empty when none fits.
std::string pick_cover(const std::vector<std::string> &image_paths);

}  // namespace vplayer
