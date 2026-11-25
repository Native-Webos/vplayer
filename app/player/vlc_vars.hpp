// SPDX-License-Identifier: GPL-3.0-or-later
//
// What libVLC 3's public API cannot do, done as libVLC itself does it: a media player is a VLC object
// (lib/media_player_internal.h: VLC_COMMON_MEMBERS first), and its "audio-filter" variable is what
// the audio output it creates at play inherits. libvlc_media_player_set_equalizer sets it to
// "equalizer" or "": set the filters after that, and before play.
#pragma once

#include <string>

struct libvlc_media_player_t;
struct libvlc_equalizer_t;

namespace vplayer {

struct AudioEffects;

// `filters`: VLC audio filter modules, ':' between them ("equalizer:normvol"), on the player and on
// the audio output it already made. False without libVLC.
bool set_player_audio_filters(libvlc_media_player_t *mp, const std::string &filters);

// A module option on the player, for the objects made under it that inherit it: the audio output
// and the stream output (media options do not reach a sout's filters, whose parent is the player,
// not the input). Created when the player lacks the variable. Before play.
bool set_player_option(libvlc_media_player_t *mp, const char *name, float value);
// An address (a callback) the same way: "vmem-sar" (libvlc/patches/vlc/0008), which the video
// output calls with the source's sample aspect ratio.
bool set_player_address(libvlc_media_player_t *mp, const char *name, void *value);

// The equalizer `fx` asks for (a preset, or its own bands), or null when off. The caller releases it
// (libvlc_audio_equalizer_release).
libvlc_equalizer_t *make_equalizer(const AudioEffects &fx);

}  // namespace vplayer
