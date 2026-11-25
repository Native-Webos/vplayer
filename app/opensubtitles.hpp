// SPDX-License-Identifier: GPL-3.0-or-later
//
// Subtitle downloads from OpenSubtitles (REST API v1, https://api.opensubtitles.com/api/v1), with the
// viewer's own API key and account (opensubtitles.com: a free account, then an API consumer for the
// key). The API is used as the open-source clients of it use it (the Python "opensubtitlescom"
// package, Jellyfin's plugin):
//
//   search    GET /subtitles?languages=..&moviehash=..&query=..  (Api-Key, User-Agent "Pylon v0.1")
//   login     POST /login {"username","password"} -> token (Bearer for downloads)
//   download  POST /download {"file_id", "sub_format":"srt"} -> link, remaining; then GET link
//
// 401: signed out (logs in again once), 406: the day's downloads used up, 429: too many requests.
// The movie hash (local files of 128 KiB or more): the size plus the 64-bit little-endian words of
// the first and last 64 KiB, as 16 hex digits.
//
// Parsing and URLs are pure and tested on the host; Client blocks (run it on a worker).
#pragma once

#include "wnk/async/async.hpp"
#include "wnk/net/http.hpp"

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace vplayer::opensubs {

struct Account {
    std::string api_key, username, password;
    bool configured() const { return !api_key.empty() && !username.empty() && !password.empty(); }
};
std::string account_json(const Account &a);
Account account_from_json(const std::string &json);

// The OpenSubtitles hash of a local file; nullopt when unreadable or under 128 KiB.
std::optional<std::string> movie_hash(const std::string &path);

struct Result {
    int64_t file_id = 0;
    std::string file_name, release, language, title;
    int downloads = 0;
    bool hearing_impaired = false, hash_match = false;
};

std::string search_url(const std::string &query, const std::string &hash, const std::string &language);
// Results with a file, the ones matching the hash first, then the most downloaded.
bool parse_search(const std::string &json, std::vector<Result> *out, std::string *error);
bool parse_login(const std::string &json, std::string *token, std::string *error);

struct Download {
    std::string link, file_name;
    int remaining = -1;      // downloads left today, when the server says
    std::string reset_time;  // when they come back ("23 hours and 10 minutes"), when it says
};
bool parse_download(const std::string &json, Download *out, std::string *error);

// What to tell the viewer about a failed request.
std::string error_text(long status, const std::string &body);

class Client {
 public:
    Client(std::shared_ptr<wnk::net::HttpTransport> http, Account account);
    // Blocking: a worker. `language`: ISO 639-1 ("en").
    bool search(const std::string &query, const std::string &hash, const std::string &language, std::vector<Result> *out,
                std::string *error, const wnk::async::CancellationToken &cancel);
    // The subtitle file's text and name; signs in first (and again once, when the token expired).
    bool download(int64_t file_id, std::string *text, Download *info, std::string *error,
                  const wnk::async::CancellationToken &cancel);

 private:
    wnk::net::HttpRequest request(const std::string &method, const std::string &path, const std::string &body = {}) const;
    bool login(std::string *error, const wnk::async::CancellationToken &cancel);

    std::shared_ptr<wnk::net::HttpTransport> http_;
    Account account_;
    std::mutex mu_;
    std::string token_;  // after login (mu_)
};

}  // namespace vplayer::opensubs
