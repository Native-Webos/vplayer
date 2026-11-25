// SPDX-License-Identifier: GPL-3.0-or-later
#include "player/vlc_vars.hpp"

#include "player/playback.hpp"

#if VPLAYER_HAVE_LIBVLC
#include <vlc/vlc.h>
#include <vlc_common.h>
#include <vlc_variables.h>

#include <cstring>
#endif

namespace vplayer {

#if VPLAYER_HAVE_LIBVLC
bool set_player_audio_filters(libvlc_media_player_t *mp, const std::string &filters) {
    if (mp == nullptr) return false;
    vlc_object_t *obj = reinterpret_cast<vlc_object_t *>(mp);
    if (var_SetString(obj, "audio-filter", filters.c_str()) != VLC_SUCCESS) return false;
    // VLC 3 makes the player's audio output with the player (lib/media_player.c), and the output
    // copied the filter list then: set it there too, as libvlc_media_player_set_equalizer does.
    vlc_list_t *children = vlc_list_children(obj);
    for (int i = 0; children != nullptr && i < children->i_count; ++i) {
        auto *child = static_cast<vlc_object_t *>(children->p_values[i].p_address);
        if (child != nullptr && child->obj.object_type != nullptr && std::strcmp(child->obj.object_type, "audio output") == 0)
            var_SetString(child, "audio-filter", filters.c_str());
    }
    if (children != nullptr) vlc_list_release(children);
    return true;
}

bool set_player_option(libvlc_media_player_t *mp, const char *name, float value) {
    if (mp == nullptr) return false;
    vlc_object_t *obj = reinterpret_cast<vlc_object_t *>(mp);
    if (var_Type(obj, name) == 0 && var_Create(obj, name, VLC_VAR_FLOAT) != VLC_SUCCESS) return false;
    return var_SetFloat(obj, name, value) == VLC_SUCCESS;
}
bool set_player_address(libvlc_media_player_t *mp, const char *name, void *value) {
    if (mp == nullptr) return false;
    vlc_object_t *obj = reinterpret_cast<vlc_object_t *>(mp);
    if (var_Type(obj, name) == 0 && var_Create(obj, name, VLC_VAR_ADDRESS) != VLC_SUCCESS) return false;
    return var_SetAddress(obj, name, value) == VLC_SUCCESS;
}
libvlc_equalizer_t *make_equalizer(const AudioEffects &fx) {
    if (fx.equalizer < 0) return nullptr;
    if (fx.equalizer != AudioEffects::kCustomEqualizer)
        return libvlc_audio_equalizer_new_from_preset(static_cast<unsigned>(fx.equalizer));
    libvlc_equalizer_t *eq = libvlc_audio_equalizer_new();
    if (eq == nullptr) return nullptr;
    libvlc_audio_equalizer_set_preamp(eq, fx.bands[0]);
    for (unsigned b = 0; b < 10 && b < libvlc_audio_equalizer_get_band_count(); ++b)
        libvlc_audio_equalizer_set_amp_at_index(eq, fx.bands[b + 1], b);
    return eq;
}
#else
libvlc_equalizer_t *make_equalizer(const AudioEffects &) { return nullptr; }
bool set_player_audio_filters(libvlc_media_player_t *, const std::string &) { return false; }
bool set_player_option(libvlc_media_player_t *, const char *, float) { return false; }
bool set_player_address(libvlc_media_player_t *, const char *, void *) { return false; }
#endif

}  // namespace vplayer
