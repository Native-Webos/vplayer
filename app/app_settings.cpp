// SPDX-License-Identifier: GPL-3.0-or-later
#include "app_settings.hpp"

#include <cctype>

#include "wnk/log.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>

namespace vplayer {

namespace {
constexpr const char *kKey = "settings";
}

const std::vector<Language> &languages() {
    static const std::vector<Language> list = {
        {"English", "en"}, {"French", "fr"},   {"German", "de"},    {"Spanish", "es"}, {"Italian", "it"},
        {"Portuguese", "pt"}, {"Dutch", "nl"}, {"Swedish", "sv"},   {"Danish", "da"},  {"Norwegian", "no"},
        {"Finnish", "fi"}, {"Polish", "pl"},   {"Russian", "ru"},   {"Turkish", "tr"}, {"Arabic", "ar"},
        {"Hindi", "hi"},   {"Nepali", "ne"},   {"Japanese", "ja"},  {"Korean", "ko"},  {"Chinese", "zh"},
    };
    return list;
}

bool language_matches(const std::string &track, const std::string &code) {
    if (track.empty() || code.empty()) return false;
    std::string t;
    for (const char c : track) {
        if (c == '-' || c == '_') break;  // "pt-BR": the language part
        t += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    if (t == code) return true;
    // ISO 639-2 (bibliographic and terminology) to 639-1, for the languages offered.
    static const char *const three[][3] = {
        {"eng", "", "en"}, {"fre", "fra", "fr"}, {"ger", "deu", "de"}, {"spa", "", "es"}, {"ita", "", "it"},
        {"por", "", "pt"}, {"dut", "nld", "nl"}, {"swe", "", "sv"},    {"dan", "", "da"},  {"nor", "nob", "no"},
        {"fin", "", "fi"}, {"pol", "", "pl"},    {"rus", "", "ru"},    {"tur", "", "tr"},  {"ara", "", "ar"},
        {"hin", "", "hi"}, {"nep", "", "ne"},    {"jpn", "", "ja"},    {"kor", "", "ko"},  {"chi", "zho", "zh"},
    };
    for (const auto &row : three)
        if (code == row[2] && (t == row[0] || (row[1][0] != 0 && t == row[1]))) return true;
    return false;
}

float subtitle_scale(int size) {
    static const float scales[] = {0.045f, 0.055f, 0.07f, 0.085f};
    return scales[size < 0 ? 0 : size > 3 ? 3 : size];
}

std::string settings_json(const AppSettings &s) {
    return nlohmann::json{{"software_only", s.software_only}, {"resume", s.resume}, {"subtitle_size", s.subtitle_size},
                          {"subtitle_box", s.subtitle_box}, {"subtitle_colour", s.subtitle_colour}, {"audio_pcm", s.audio_pcm},
                          {"disc_menus", s.disc_menus},
                          {"audio_language", s.audio_language}, {"subtitle_language", s.subtitle_language}}
        .dump();
}

AppSettings settings_from_json(const std::string &text) {
    AppSettings s;
    const auto j = nlohmann::json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return s;
    if (auto it = j.find("software_only"); it != j.end() && it->is_boolean()) s.software_only = it->get<bool>();
    if (auto it = j.find("resume"); it != j.end() && it->is_boolean()) s.resume = it->get<bool>();
    if (auto it = j.find("subtitle_size"); it != j.end() && it->is_number_integer())
        s.subtitle_size = std::clamp(it->get<int>(), 0, 3);
    if (auto it = j.find("subtitle_box"); it != j.end() && it->is_boolean()) s.subtitle_box = it->get<bool>();
    if (auto it = j.find("subtitle_colour"); it != j.end() && it->is_number_integer())
        s.subtitle_colour = std::clamp(it->get<int>(), 0, 1);
    if (auto it = j.find("audio_pcm"); it != j.end() && it->is_boolean()) s.audio_pcm = it->get<bool>();
    if (auto it = j.find("disc_menus"); it != j.end() && it->is_boolean()) s.disc_menus = it->get<bool>();
    if (auto it = j.find("audio_language"); it != j.end() && it->is_string()) s.audio_language = it->get<std::string>();
    if (auto it = j.find("subtitle_language"); it != j.end() && it->is_string())
        s.subtitle_language = it->get<std::string>();
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
