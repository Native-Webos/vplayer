// SPDX-License-Identifier: GPL-3.0-or-later
//
// Local media for the Home and Browse screens: the USB drives the TV has mounted, and the newest
// videos on them. Built on the kit's wnk/platform/files.hpp (which reads /tmp/usb on the TV).
#pragma once

#include "wnk/platform/files.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace vplayer {

// The removable volumes to browse. On a desktop, VPLAYER_EXTRA_ROOT=/some/folder adds that folder as
// one more volume (for host runs and screenshots); the TV never sets it.
std::vector<wnk::platform::RemovableVolume> media_volumes();

// The same mount points in the same order (to notice drives coming and going).
bool same_volumes(const std::vector<wnk::platform::RemovableVolume> &a,
                  const std::vector<wnk::platform::RemovableVolume> &b);

// The newest video files on `volumes`, newest first. Walks at most `max_depth` folders deep and
// looks at no more than `max_entries` entries in all, so a huge drive cannot stall it. Blocking:
// call it on a worker.
std::vector<wnk::platform::FileEntry> recent_videos(const std::vector<wnk::platform::RemovableVolume> &volumes,
                                                    size_t count, int max_depth = 3, size_t max_entries = 4000);

// Every file of the given kinds on `volumes` (and folders deep), in folder order. Bounded like
// recent_videos. Blocking: a worker.
std::vector<wnk::platform::FileEntry> scan_files(const std::vector<wnk::platform::RemovableVolume> &volumes,
                                                 const std::vector<wnk::platform::FileKind> &kinds, int max_depth = 4,
                                                 size_t max_entries = 20000);

// "4.2 GB", "731 MB", "12 KB".
std::string format_bytes(uint64_t bytes);
// "MKV" for "Film.mkv"; empty without an extension.
std::string extension_upper(const std::string &name);
// Discs: images (.iso, .img) and disc folders, which play through VLC's "dvd://" or "bluray://"
// access. disc_folder_mrl: the address that plays `folder` as a disc when its entries hold a
// VIDEO_TS (DVD) or BDMV (Blu-ray) folder; empty otherwise.
bool is_disc(const wnk::platform::FileEntry &e);
std::string disc_folder_mrl(const std::string &folder, const std::vector<wnk::platform::FileEntry> &entries);
// "DVD" or "BD" for a disc folder's address; empty for anything else.
std::string disc_folder_kind(const std::string &path);

// The file name without its extension, for titles ("Film (2025)" for "Film (2025).mkv").
std::string display_name(const std::string &name);
// A position for people: "4:05", "1:02:03".
std::string clock_text(int64_t ms);
// What people type for a position: "1:23:45", "83:10", "95" (seconds), "1.23.45" or "1 23 45" with
// any separators; -1 when it is not one.
int64_t parse_clock(const std::string &text);

}  // namespace vplayer
