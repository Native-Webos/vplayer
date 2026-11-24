// SPDX-License-Identifier: GPL-3.0-or-later
//
// Music from the drives, grouped the way people file it: a folder of audio files is an album, its
// parent folder the artist, and a cover image in it (cover/folder/front/albumart*.jpg|png, else the
// folder's only image) its artwork. Tags are not read (that would mean opening every file).
// Pure, tested on the host.
#pragma once

#include "wnk/platform/files.hpp"

#include <string>
#include <vector>

namespace vplayer {

struct Album {
    std::string title;   // the folder's name
    std::string artist;  // the parent folder's name; empty at a drive's root
    std::string folder;
    std::string cover;   // an image path, or empty
    std::vector<wnk::platform::FileEntry> tracks;  // natural order
};

// `files`: audio files and images from scan_files. Albums in natural order of artist, then title.
std::vector<Album> build_albums(const std::vector<wnk::platform::FileEntry> &files);
// The cover among a folder's image names (lower-case comparisons); empty when none fits.
std::string pick_cover(const std::vector<std::string> &image_paths);

}  // namespace vplayer
