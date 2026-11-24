// SPDX-License-Identifier: GPL-3.0-or-later
//
// CEA-708 (DTVCC) closed captions, service 1, for the hardware path: the digital captions ATSC
// broadcasts carry beside CEA-608 in the same A/53 user data (cc_type 2 starts a DTVCC packet, 3
// continues it).
//
//   cc708_bytes      the DTVCC byte pairs of an H.264/HEVC access unit, as (start, b1, b2)
//   Cea708Decoder    packets -> service blocks -> service 1's commands: windows defined (DFx),
//                    shown and hidden (DSW, HDW, TGW), cleared (CLW) and deleted (DLW), the current
//                    window (CWx), the pen moved (SPL); text in G0 (ASCII, 0x7F a music note), G1
//                    (Latin-1) and the extended sets' common characters; CR, BS, FF, HCR. Pen and
//                    window styles are read past, not drawn. Each change of the visible windows'
//                    text becomes a cue (until the next); windows show in their priority order.
//
// Pure, tested on the host. Pairs must come in presentation order (the source reorders them).
#pragma once

#include "wnk/media/media.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace vplayer {

struct Cc708Pair {
    bool start = false;  // cc_type 3: a new DTVCC packet starts with this pair
    uint8_t b1 = 0, b2 = 0;
};
std::vector<Cc708Pair> cc708_pairs(const uint8_t *annexb, size_t size, bool hevc);
// Both kinds in one pass over the access unit (either output may be null).
void cc_pairs(const uint8_t *annexb, size_t size, bool hevc, std::vector<std::pair<uint8_t, uint8_t>> *cc608,
              std::vector<Cc708Pair> *cc708);

class Cea708Decoder {
 public:
    using Cue = std::shared_ptr<const wnk::media::SubtitleCue>;
    explicit Cea708Decoder(int track) : track_(track) {}
    // One pair at `pts_us`: the cues it makes (when what shows changed: at once for windows shown,
    // hidden or cleared and new lines; typed text at most every 250 ms).
    std::vector<Cue> feed(const Cc708Pair &pair, int64_t pts_us);
    // Each access unit, after its pairs: text typed and not yet shown, once 250 ms have passed.
    std::vector<Cue> tick(int64_t pts_us);
    void reset();
    // A service-1 command was seen since the last reset (the stream has 708 captions).
    bool seen() const { return seen_; }

 private:
    struct Window {
        bool defined = false, visible = false;
        int priority = 0;
        int rows = 1, cols = 32;
        int row = 0, col = 0;
        std::vector<std::u32string> text;  // rows
    };
    void packet(const std::vector<uint8_t> &p, int64_t pts_us);
    void service(const uint8_t *d, size_t n);
    void put(char32_t c);
    void define(int id, const uint8_t *p);
    Cue cue(int64_t pts_us) const;
    Window *current() { return cur_ >= 0 ? &win_[static_cast<size_t>(cur_)] : nullptr; }

    int track_;
    std::vector<uint8_t> buf_;  // the packet being gathered
    size_t want_ = 0;           // its size, from its header
    std::array<Window, 8> win_{};
    int cur_ = -1;
    bool changed_ = false, urgent_ = false, seen_ = false;
    int64_t last_cue_us_ = wnk::media::kNoTime;
    std::vector<Cue> out_;
};

}  // namespace vplayer
