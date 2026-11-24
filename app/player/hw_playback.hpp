// SPDX-License-Identifier: GPL-3.0-or-later
//
// Hardware playback: libVLC demuxes (VlcSource, through the wnk_bridge stream output) and the TV
// decodes and presents on its video plane, through webos-native-app-kit's PlaybackController and
// its Starfish / NDL backends (AutoBackend: Starfish for H.264/HEVC, NDL for the rest).
//
// The picture is on a plane under the UI: the player screen clears the UI to transparent and draws
// only its controls. 4K, HDR10, HLG and Dolby Vision play here; the software path cannot.
//
// Subtitles: libVLC delivers the selected subtitle stream through the bridge; text formats become
// cues in the controller's store, which the player screen draws over the picture (subtitle_cues).
// Bitmap formats (PGS, DVD, DVB) are listed but not shown yet.
//
// Not yet on this path: chapters, video filters and the equalizer (the TV decodes; VLC never sees
// the pictures or the samples).
#pragma once

#include "player/playback.hpp"

#include "wnk/async/async.hpp"

#include <memory>
#include <string>
#include <vector>

struct libvlc_instance_t;

namespace wnk::media {
class PlaybackController;
}

namespace vplayer {

// What the TV can be told about (TV settings: Dolby Vision, Atmos). Detected once, on a worker.
struct HwOptions {
    bool dolby_vision = false;
    bool atmos = false;
};

// Whether this device has a hardware decoder the kit can drive (Starfish or NDL). Cached.
bool hw_playback_available(std::string *why = nullptr);

std::unique_ptr<Playback> open_hw_playback(libvlc_instance_t *vlc, const std::string &path,
                                           wnk::async::MainDispatcher &main, const HwOptions &options,
                                           std::vector<std::string> vlc_options = {});

}  // namespace vplayer
