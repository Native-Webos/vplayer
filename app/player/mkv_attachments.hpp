// SPDX-License-Identifier: GPL-3.0-or-later
//
// The fonts a Matroska file carries for its styled (ASS/SSA) subtitles, which libVLC does not hand
// out: read from the file's Attachments element, found among the Segment's first children or
// through its SeekHead (mkvmerge often writes attachments at the end of the file). Local files.
//
// Fonts are attachments with a font MIME type ("font/ttf", "application/x-truetype-font",
// "application/vnd.ms-opentype"...) or a .ttf/.otf/.ttc/.woff name; at most 64 of them and 48 MB.
#pragma once

#include "wnk/media/media.hpp"

#include <string>
#include <vector>

namespace vplayer {

bool is_font_attachment(const std::string &name, const std::string &mime);

// The fonts attached to the Matroska file at `path`; empty when none, or not Matroska. Blocking.
std::vector<wnk::media::FontAttachment> mkv_fonts(const std::string &path);

}  // namespace vplayer
