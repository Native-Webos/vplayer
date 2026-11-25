// SPDX-License-Identifier: GPL-3.0-or-later
//
// The viewer's settings, kept in the app's key/value store as JSON:
//   software_only   decode with libVLC only, never the TV's decoder ("Decoder" in Settings)
//   resume          start files where they stopped ("Continue where I stopped")
//   subtitle_size, subtitle_box   how text subtitles look (both decoding paths)
#pragma once

#include "wnk/async/async.hpp"
#include "wnk/storage/kv.hpp"

#include <string>
#include <vector>

namespace vplayer {

struct AppSettings {
    bool software_only = false;
    bool resume = true;
    int subtitle_size = 1;      // 0 small, 1 normal, 2 large, 3 huge
    bool subtitle_box = false;  // on a dark box instead of an outline
    int subtitle_colour = 0;    // 0 white, 1 yellow
    bool audio_pcm = false;     // the TV decoder always gets decoded PCM (no passthrough, no AC-3)
    bool disc_menus = true;     // DVD and Blu-ray images start at their menu (the software path)
    std::string audio_language;     // ISO 639-1 ("en"); empty: the file's default track
    std::string subtitle_language;  // ISO 639-1; "any": the first track; empty: as the file says
};

// The languages offered in Settings (name, ISO 639-1 code), in the order shown.
struct Language {
    const char *name;
    const char *code;
};
const std::vector<Language> &languages();
// Whether a track's language tag ("eng", "en", "ger", "deu", "pt-BR"...) is `code` (ISO 639-1).
bool language_matches(const std::string &track_language, const std::string &code);

// Text height as a fraction of the picture's, for each subtitle_size.
float subtitle_scale(int size);

std::string settings_json(const AppSettings &s);
AppSettings settings_from_json(const std::string &json);
// Blocking (a worker).
AppSettings read_settings(wnk::storage::KeyValueStore *store);
void save_settings(wnk::storage::KeyValueStore *store, const AppSettings &s, wnk::async::WorkerPool &pool);

}  // namespace vplayer
