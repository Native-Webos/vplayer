// SPDX-License-Identifier: GPL-3.0-or-later
//
// CEA-608 closed captions (US broadcast, carried in the video as ATSC A/53 "GA94" user data) for
// the hardware path, where VLC does not decode the video and so never extracts them.
//
//   cc608_pairs     the field-1 byte pairs of an H.264 or HEVC access unit (Annex-B): SEI messages
//                   of type 4 (ITU-T T.35, country 0xB5, provider 0x0031, "GA94", type 3: cc_data),
//                   emulation-prevention bytes removed; cc_type 0 only (CC1/CC2)
//   Cea608Decoder   channel 1 (CC1) on a 15x32 screen: pop-on (loaded off screen, shown at End Of
//                   Caption), roll-up (2-4 rows, scrolled by Carriage Return) and paint-on;
//                   preamble address codes place the cursor, mid-row codes are spaces, the special
//                   and extended characters replace what the basic set lacks. Each change of what
//                   shows becomes a cue (until the next); Erase Displayed Memory clears.
//
// Pairs must be fed in presentation order (the source reorders access units by their time).
// Pure, tested on the host.
#pragma once

#include "wnk/media/media.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace vplayer {

using CcPair = std::pair<uint8_t, uint8_t>;
std::vector<CcPair> cc608_pairs(const uint8_t *annexb, size_t size, bool hevc);

class Cea608Decoder {
 public:
    using Cue = std::shared_ptr<const wnk::media::SubtitleCue>;
    explicit Cea608Decoder(int track) : track_(track) {}
    // One pair at `pts_us`: the cue it makes showing (when what shows changed), else nothing.
    std::vector<Cue> feed(uint8_t b1, uint8_t b2, int64_t pts_us);
    void reset();

 private:
    using Screen = std::array<std::u32string, 15>;
    enum class Mode : uint8_t { PopOn, RollUp, PaintOn };
    void put(char32_t c);
    Screen &target() { return mode_ == Mode::PopOn ? hidden_ : shown_; }
    Cue cue(int64_t pts_us) const;

    int track_;
    Mode mode_ = Mode::PopOn;
    Screen shown_{}, hidden_{};
    int row_ = 14, col_ = 0;
    int rollup_rows_ = 2;
    CcPair last_control_{0, 0};  // control codes come twice: the copy is skipped
    bool changed_ = false, urgent_ = false;
    int64_t last_cue_us_ = wnk::media::kNoTime;
};

}  // namespace vplayer
