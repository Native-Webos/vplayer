// SPDX-License-Identifier: GPL-3.0-or-later
// Settings: languages and their saved form (app_settings.hpp).
#define WNK_TEST_MAIN
#include "wnk_test.hpp"

#include "app_settings.hpp"

#include <string>

using namespace vplayer;

TEST("language_matches") {
    CHECK(language_matches("eng", "en"));
    CHECK(language_matches("en", "en"));
    CHECK(language_matches("EN", "en"));
    CHECK(language_matches("ger", "de"));
    CHECK(language_matches("deu", "de"));
    CHECK(language_matches("fre", "fr"));
    CHECK(language_matches("fra", "fr"));
    CHECK(language_matches("pt-BR", "pt"));
    CHECK(language_matches("nep", "ne"));
    CHECK(!language_matches("fre", "en"));
    CHECK(!language_matches("", "en"));
    CHECK(!language_matches("und", "en"));
    CHECK(!language_matches("eng", ""));
}

TEST("languages_listed_have_codes") {
    for (const auto &l : languages()) {
        CHECK(std::string(l.code).size() == 2);
        CHECK(language_matches(l.code, l.code));
    }
}

TEST("settings_json_languages") {
    AppSettings s;
    s.audio_language = "fr";
    s.subtitle_language = "any";
    const AppSettings back = settings_from_json(settings_json(s));
    CHECK_EQ(back.audio_language, std::string("fr"));
    CHECK_EQ(back.subtitle_language, std::string("any"));
    const AppSettings old = settings_from_json("{\"resume\":false}");  // saved before languages existed
    CHECK(old.audio_language.empty());
    CHECK(old.subtitle_language.empty());
    CHECK(!old.resume);
}
