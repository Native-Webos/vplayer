// SPDX-License-Identifier: GPL-3.0-or-later
//
// Teletext subtitles (EBU, as broadcast in DVB transport streams: VLC's 'telx'), decoded into text
// cues for the hardware path. Each PES carries EBU data units (bits sent least significant first,
// so every byte is reversed): Hamming 8/4 coded addresses (magazine, row), page headers (page
// number, erase and subtitle flags) and rows of 40 odd-parity characters. The page to show is the
// one VLC's stream names (magazine, page), else the first page flagged as subtitles.
//
// A row's text is what lies between its start-box and end-box codes (subtitle pages box their text),
// else the whole row; control codes become spaces. A cue goes out at the end of each PES that
// brought rows of the page (a later, fuller one replaces it); a header with no rows clears.
// Pure, tested on the host.
#pragma once

#include "wnk/media/media.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace vplayer {

class TeletextDecoder {
 public:
    using Cue = std::shared_ptr<const wnk::media::SubtitleCue>;
    // `magazine` 1..8 and `page` (tens << 4 | units), or magazine -1: the first subtitle page.
    TeletextDecoder(int magazine, int page);
    std::vector<Cue> decode(const uint8_t *data, size_t size, int track, int64_t pts_us);

    static uint8_t reverse(uint8_t b);
    static int unham(uint8_t b);  // Hamming 8/4 data bits, no correction

 private:
    Cue cue(int track, int64_t pts_us) const;

    int magazine_, page_;
    bool on_page_ = false;  // rows that follow belong to the page
    bool pending_ = false;  // a header or rows not yet sent as a cue
    std::array<std::string, 24> rows_;
};

}  // namespace vplayer
