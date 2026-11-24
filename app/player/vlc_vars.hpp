// SPDX-License-Identifier: GPL-3.0-or-later
//
// What libVLC 3's public API cannot do, done as libVLC itself does it: a media player is a VLC object
// (lib/media_player_internal.h: VLC_COMMON_MEMBERS first), and its "audio-filter" variable is what
// the audio output it creates at play inherits. libvlc_media_player_set_equalizer sets it to
// "equalizer" or "": set the filters after that, and before play.
#pragma once

#include <string>

struct libvlc_media_player_t;

namespace vplayer {

// `filters`: VLC audio filter modules, ':' between them ("equalizer:normvol"). False without libVLC.
bool set_player_audio_filters(libvlc_media_player_t *mp, const std::string &filters);

}  // namespace vplayer
