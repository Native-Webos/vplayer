// SPDX-License-Identifier: GPL-3.0-or-later
#include "app_settings.hpp"

#include "wnk/log.hpp"

#include <nlohmann/json.hpp>

namespace vplayer {

namespace {
constexpr const char *kKey = "settings";
}

std::string settings_json(const AppSettings &s) {
    return nlohmann::json{{"software_only", s.software_only}, {"resume", s.resume}}.dump();
}

AppSettings settings_from_json(const std::string &text) {
    AppSettings s;
    const auto j = nlohmann::json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return s;
    if (auto it = j.find("software_only"); it != j.end() && it->is_boolean()) s.software_only = it->get<bool>();
    if (auto it = j.find("resume"); it != j.end() && it->is_boolean()) s.resume = it->get<bool>();
    return s;
}

AppSettings read_settings(wnk::storage::KeyValueStore *store) {
    if (store == nullptr) return {};
    wnk::storage::Entry e;
    if (store->get(kKey, &e) != wnk::storage::Status::Ok) return {};
    return settings_from_json(e.value);
}

void save_settings(wnk::storage::KeyValueStore *store, const AppSettings &s, wnk::async::WorkerPool &pool) {
    if (store == nullptr) return;
    pool.submit([store, json = settings_json(s)](const wnk::async::CancellationToken &) {
        if (store->put(kKey, json).status != wnk::storage::Status::Ok) WNK_LOGW("settings", "settings not stored");
    });
}

}  // namespace vplayer
