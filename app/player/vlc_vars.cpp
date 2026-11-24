// SPDX-License-Identifier: GPL-3.0-or-later
#include "player/vlc_vars.hpp"

#if VPLAYER_HAVE_LIBVLC
#include <vlc/vlc.h>
#include <vlc_common.h>
#include <vlc_variables.h>
#endif

namespace vplayer {

#if VPLAYER_HAVE_LIBVLC
bool set_player_audio_filters(libvlc_media_player_t *mp, const std::string &filters) {
    if (mp == nullptr) return false;
    vlc_object_t *obj = reinterpret_cast<vlc_object_t *>(mp);
    return var_SetString(obj, "audio-filter", filters.c_str()) == VLC_SUCCESS;
}
#else
bool set_player_audio_filters(libvlc_media_player_t *, const std::string &) { return false; }
#endif

}  // namespace vplayer
