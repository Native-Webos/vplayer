// SPDX-License-Identifier: GPL-3.0-or-later
#include "opensubtitles.hpp"

#include "wnk/log.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdio>
#include <fstream>

namespace vplayer::opensubs {

namespace {

constexpr const char *kCat = "opensubs";
constexpr const char *kBase = "https://api.opensubtitles.com/api/v1";
constexpr const char *kUserAgent = "VPlayer v0.1";
using json = nlohmann::json;

std::string url_encode(const std::string &s) {
    static const char hex[] = "0123456789ABCDEF";
    std::string out;
    for (const char ch : s) {
        const auto c = static_cast<unsigned char>(ch);
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
            c == '.' || c == '~') {
            out += ch;
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

std::string str(const json &j, const char *key) {
    const auto it = j.find(key);
    return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
}

}  // namespace

std::string account_json(const Account &a) {
    return json{{"api_key", a.api_key}, {"username", a.username}, {"password", a.password}}.dump();
}

Account account_from_json(const std::string &text) {
    Account a;
    const auto j = json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return a;
    a.api_key = str(j, "api_key");
    a.username = str(j, "username");
    a.password = str(j, "password");
    return a;
}

std::optional<std::string> movie_hash(const std::string &path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return std::nullopt;
    f.seekg(0, std::ios::end);
    const auto size = static_cast<uint64_t>(f.tellg());
    constexpr uint64_t kChunk = 65536;
    if (size < 2 * kChunk) return std::nullopt;
    uint64_t hash = size;
    auto add = [&](uint64_t from) {
        f.seekg(static_cast<std::streamoff>(from));
        unsigned char buf[kChunk];
        if (!f.read(reinterpret_cast<char *>(buf), kChunk)) return false;
        for (size_t i = 0; i < kChunk; i += 8) {
            uint64_t w = 0;
            for (int b = 7; b >= 0; --b) w = w << 8 | buf[i + static_cast<size_t>(b)];  // little-endian
            hash += w;
        }
        return true;
    };
    if (!add(0) || !add(size - kChunk)) return std::nullopt;
    char out[17];
    std::snprintf(out, sizeof out, "%016llx", static_cast<unsigned long long>(hash));
    return std::string(out);
}

std::string search_url(const std::string &query, const std::string &hash, const std::string &language) {
    // Parameters in alphabetical order, lower case (as the API prefers, and Jellyfin sends them).
    std::string q;
    std::string lower = query;
    for (auto &c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    std::string lang = language;
    for (auto &c : lang) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (!lang.empty()) q += "languages=" + url_encode(lang);
    if (!hash.empty()) q += std::string(q.empty() ? "" : "&") + "moviehash=" + url_encode(hash);
    if (!lower.empty()) q += std::string(q.empty() ? "" : "&") + "query=" + url_encode(lower);
    return std::string(kBase) + "/subtitles?" + q;
}

bool parse_search(const std::string &text, std::vector<Result> *out, std::string *error) {
    out->clear();
    const auto j = json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object() || !j.contains("data") || !j["data"].is_array()) {
        *error = "an answer VPlayer does not understand";
        return false;
    }
    for (const auto &d : j["data"]) {
        const auto a = d.value("attributes", json::object());
        if (!a.is_object() || !a.contains("files") || !a["files"].is_array() || a["files"].empty()) continue;
        const auto &file = a["files"][0];
        Result r;
        r.file_id = file.value("file_id", int64_t{0});
        if (r.file_id == 0) continue;
        r.file_name = str(file, "file_name");
        r.release = str(a, "release");
        r.language = str(a, "language");
        r.downloads = a.value("download_count", 0);
        r.hearing_impaired = a.value("hearing_impaired", false);
        r.hash_match = a.value("moviehash_match", false);
        if (a.contains("feature_details") && a["feature_details"].is_object()) {
            const auto &f = a["feature_details"];
            r.title = str(f, "title");
            if (r.title.empty()) r.title = str(f, "movie_name");
        }
        out->push_back(std::move(r));
    }
    std::stable_sort(out->begin(), out->end(), [](const Result &x, const Result &y) {
        if (x.hash_match != y.hash_match) return x.hash_match;
        return x.downloads > y.downloads;
    });
    return true;
}

bool parse_login(const std::string &text, std::string *token, std::string *error) {
    const auto j = json::parse(text, nullptr, false);
    *token = j.is_object() ? str(j, "token") : std::string();
    if (token->empty()) *error = "the sign-in gave no token";
    return !token->empty();
}

bool parse_download(const std::string &text, Download *out, std::string *error) {
    const auto j = json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        *error = "an answer VPlayer does not understand";
        return false;
    }
    out->link = str(j, "link");
    out->file_name = str(j, "file_name");
    out->remaining = j.value("remaining", -1);
    out->reset_time = str(j, "reset_time");
    if (out->link.empty()) {
        const std::string message = str(j, "message");
        *error = message.empty() ? "no download link" : message;
        return false;
    }
    return true;
}

std::string error_text(long status, const std::string &body) {
    std::string message;
    const auto j = json::parse(body, nullptr, false);
    if (j.is_object()) {
        message = str(j, "message");
        if (message.empty() && j.contains("errors") && j["errors"].is_array() && !j["errors"].empty() && j["errors"][0].is_string())
            message = j["errors"][0].get<std::string>();
    }
    switch (status) {
        case 401: return "OpenSubtitles did not accept the sign-in (user name, password or API key?)";
        case 403: return "OpenSubtitles refused the API key";
        case 406: return "today's downloads are used up" + (message.empty() ? std::string() : " (" + message + ")");
        case 429: return "too many requests to OpenSubtitles: try again in a moment";
        default: break;
    }
    return message.empty() ? "OpenSubtitles answered " + std::to_string(status) : message;
}

Client::Client(std::shared_ptr<wnk::net::HttpTransport> http, Account account)
    : http_(std::move(http)), account_(std::move(account)) {}

wnk::net::HttpRequest Client::request(const std::string &method, const std::string &path, const std::string &body) const {
    wnk::net::HttpRequest r;
    r.method = method;
    r.url = path.rfind("https://", 0) == 0 ? path : std::string(kBase) + path;
    r.headers = {{"Api-Key", account_.api_key}, {"User-Agent", kUserAgent}, {"Accept", "application/json"}};
    if (!body.empty()) {
        r.headers.emplace_back("Content-Type", "application/json");
        r.body = body;
        r.idempotent = false;
    }
    r.timeout_ms = 20000;
    return r;
}

bool Client::search(const std::string &query, const std::string &hash, const std::string &language, std::vector<Result> *out,
                    std::string *error, const wnk::async::CancellationToken &cancel) {
    if (!http_) {
        *error = "this device cannot fetch addresses";
        return false;
    }
    auto req = request("GET", "");
    req.url = search_url(query, hash, language);
    const auto resp = http_->perform(req, cancel);
    if (resp.error != wnk::net::NetError::None) {
        *error = std::string("network: ") + wnk::net::error_name(resp.error);
        return false;
    }
    if (!resp.ok()) {
        *error = error_text(resp.status, resp.body);
        return false;
    }
    const bool ok = parse_search(resp.body, out, error);
    WNK_LOGI(kCat, "search (%s, %s): %zu result(s)", language.c_str(), hash.empty() ? "by name" : "by hash and name", out->size());
    return ok;
}

bool Client::login(std::string *error, const wnk::async::CancellationToken &cancel) {
    const std::string body = json{{"username", account_.username}, {"password", account_.password}}.dump();
    const auto resp = http_->perform(request("POST", "/login", body), cancel);
    if (resp.error != wnk::net::NetError::None) {
        *error = std::string("network: ") + wnk::net::error_name(resp.error);
        return false;
    }
    if (!resp.ok()) {
        *error = error_text(resp.status, resp.body);
        WNK_LOGW(kCat, "sign-in refused: %ld", resp.status);
        return false;
    }
    std::string token;
    if (!parse_login(resp.body, &token, error)) return false;
    std::lock_guard<std::mutex> lock(mu_);
    token_ = token;
    WNK_LOGI(kCat, "signed in");
    return true;
}

bool Client::download(int64_t file_id, std::string *text, Download *info, std::string *error,
                      const wnk::async::CancellationToken &cancel) {
    if (!http_) {
        *error = "this device cannot fetch addresses";
        return false;
    }
    for (int attempt = 0; attempt < 2; ++attempt) {
        std::string token;
        {
            std::lock_guard<std::mutex> lock(mu_);
            token = token_;
        }
        if (token.empty()) {
            if (!login(error, cancel)) return false;
            std::lock_guard<std::mutex> lock(mu_);
            token = token_;
        }
        auto req = request("POST", "/download", json{{"file_id", file_id}, {"sub_format", "srt"}}.dump());
        req.headers.emplace_back("Authorization", "Bearer " + token);
        const auto resp = http_->perform(req, cancel);
        if (resp.error != wnk::net::NetError::None) {
            *error = std::string("network: ") + wnk::net::error_name(resp.error);
            return false;
        }
        if (resp.status == 401 && attempt == 0) {  // the token expired: sign in again, once
            std::lock_guard<std::mutex> lock(mu_);
            token_.clear();
            continue;
        }
        if (!resp.ok()) {
            *error = error_text(resp.status, resp.body);
            return false;
        }
        if (!parse_download(resp.body, info, error)) return false;
        // The file itself: a plain GET of the link (no API headers).
        wnk::net::HttpRequest get;
        get.url = info->link;
        get.headers = {{"User-Agent", kUserAgent}};
        get.timeout_ms = 20000;
        get.max_response_bytes = 4u << 20;
        const auto file = http_->perform(get, cancel);
        if (!file.ok()) {
            *error = file.error != wnk::net::NetError::None ? std::string("network: ") + wnk::net::error_name(file.error)
                                                            : "the file answered " + std::to_string(file.status);
            return false;
        }
        *text = file.body;
        WNK_LOGI(kCat, "downloaded %s (%zu bytes), %d left today", info->file_name.c_str(), text->size(), info->remaining);
        return true;
    }
    *error = "OpenSubtitles did not accept the sign-in";
    return false;
}

}  // namespace vplayer::opensubs
