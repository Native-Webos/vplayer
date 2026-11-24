// SPDX-License-Identifier: GPL-3.0-or-later
//
// Text subtitles as libVLC's demuxers hand them out (through the wnk_bridge stream output), turned
// into webos-native-app-kit cues for the hardware path, where VLC does not decode or draw them.
//
//   'subt'  SubRip and plain text (Matroska S_TEXT/UTF8, subtitle files, MP4 'text'): HTML-like
//           tags (<i>, <font ...>) and ASS override blocks ({\an8}) are removed
//   'ssa '  ASS/SSA events "ReadOrder,Layer,Style,Name,MarginL,MarginR,MarginV,Effect,Text" (the
//           Matroska form; VLC's subtitle file reader produces the same); the stream's setup data is
//           the script header, for styled rendering with libass
//   'tx3g'  3GPP timed text (MP4): a 16-bit length, the text, then style boxes (ignored)
//   'wvtt'  WebVTT as ISO boxes (MP4, and Matroska/WebM, which VLC repacks): vttc → payl; vtte is
//           an empty cue
//
// Text that is not valid UTF-8 and has no declared encoding is read as Windows-1252, as VLC's own
// subtitle decoder does by default. Everything here is pure: no libVLC, testable on the host.
#pragma once

#include "wnk/media/media.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace vplayer {

// Whether the hardware path can show a subtitle codec (a VLC fourcc), and how (Bitmap: see
// subtitle_bitmap.hpp).
wnk::media::SubtitleKind subtitle_kind(uint32_t vlc_fourcc);

// One packet → a cue of `track` from `start_us`, lasting `length_us` (0: until the next cue).
// nullptr if the codec is not a text one this file handles or the packet is malformed. An empty
// payload gives an empty cue, which clears the track.
std::shared_ptr<const wnk::media::SubtitleCue> text_subtitle_cue(uint32_t vlc_fourcc, const uint8_t *data, size_t size,
                                                                 const char *encoding, int track, int64_t start_us,
                                                                 int64_t length_us);

// SubRip lines with ASS override tags ({\an8} to place them at the top, colours...) are also given as
// an ASS event, drawn by libass with this script header (styled as the plain text is); lines
// without tags stay plain text. Empty when the line has no override tag.
std::string subrip_ass_header(float scale = 0.055f, bool box = false);
std::string subrip_ass_event(const std::string &utf8_text);

// Helpers, exposed for tests.
std::string subtitle_utf8(const std::string &text, const char *encoding);  // valid UTF-8 out
std::string subrip_plain_text(const std::string &text);                    // tags and {\...} removed

}  // namespace vplayer
