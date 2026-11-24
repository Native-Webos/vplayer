// SPDX-License-Identifier: GPL-3.0-or-later
//
// The viewer's settings, kept in the app's key/value store as JSON:
//   software_only   decode with libVLC only, never the TV's decoder ("Decoder" in Settings)
//   resume          start files where they stopped ("Continue where I stopped")
#pragma once

#include "wnk/async/async.hpp"
#include "wnk/storage/kv.hpp"

#include <string>

namespace vplayer {

struct AppSettings {
    bool software_only = false;
    bool resume = true;
};

std::string settings_json(const AppSettings &s);
AppSettings settings_from_json(const std::string &json);
// Blocking (a worker).
AppSettings read_settings(wnk::storage::KeyValueStore *store);
void save_settings(wnk::storage::KeyValueStore *store, const AppSettings &s, wnk::async::WorkerPool &pool);

}  // namespace vplayer
