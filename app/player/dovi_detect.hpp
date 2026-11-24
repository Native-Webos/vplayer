// SPDX-License-Identifier: GPL-3.0-or-later
//
// Dolby Vision detection for the hardware path. VLC 3 does not report Dolby Vision, so the app
// finds it itself:
//
//   find_dovi_config   the configuration record (dvcC/dvvC, ETSI GS CCM 001) in the file's header:
//                      Matroska's BlockAdditionMapping of the first video track (what mkvmerge and
//                      FFmpeg write) or the HEVC sample entry's dvcC/dvvC box in MP4. Local files.
//   scan_dovi_nals     what an HEVC access unit carries: RPUs (UNSPEC62) and an enhancement layer
//                      (UNSPEC63, or NAL units with nuh_layer_id > 0), for files without a record
//                      (older remuxes)
//   dovi_plan          from either: the profile to declare to the TV, and whether the stream must be
//                      converted from Profile 7 (dual layer) to 8.1 on the way (wnk/media/dovi.hpp)
//
// Pure: no libVLC, testable on the host.
#pragma once

#include "wnk/media/media.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace vplayer {

struct DoviConfig {
    int profile = -1;  // dv_profile; -1: none found
    int level = 0;
    bool rpu = false, el = false, bl = false;
    int bl_compatibility = 0;  // dv_bl_signal_compatibility_id: 1 HDR10, 2 SDR, 4 HLG, 6 Blu-ray HDR10
};

// A DOVIDecoderConfigurationRecord (24 bytes; the first 5 matter).
bool parse_dovi_record(const uint8_t *data, size_t size, DoviConfig *out);

// The record of the file's first video track. False when the file has none or is not MKV/MP4.
bool find_dovi_config(const std::string &path, DoviConfig *out);

struct DoviNals {
    bool rpu = false;  // UNSPEC62 present
    bool el = false;   // enhancement layer present
};
DoviNals scan_dovi_nals(const uint8_t *annexb, size_t size);

struct DoviPlan {
    int profile = -1;           // VideoFormat::dolby_vision_profile; -1: play the base layer only
    bool convert_p7 = false;    // feed through hevc_dovi_p7_to_p81
    std::string how;            // for the log and the Info panel
};
// `record` from the file (profile -1 when none), `nals` from the first keyframe, `colour` of the
// base layer, `can_convert` whether libdovi is in the build.
DoviPlan dovi_plan(const DoviConfig &record, const DoviNals &nals, const wnk::media::VideoColour &colour,
                   bool can_convert);

}  // namespace vplayer
