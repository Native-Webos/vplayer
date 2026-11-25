// SPDX-License-Identifier: GPL-3.0-or-later
// OpenSubtitles downloads (opensubtitles.hpp): the hash, URLs, answers, and the client's flow
// against a fake server.
#define WNK_TEST_MAIN
#include "wnk_test.hpp"

#include "opensubtitles.hpp"

#include <cstdio>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <string>

using namespace vplayer::opensubs;

TEST("movie_hash") {
    // 200 000 bytes of (i*7+3) & 0xFF; the expected value computed independently (Python, the
    // reference clients' algorithm).
    const std::string path = "/tmp/vplayer-test-hash.bin";
    {
        std::ofstream f(path, std::ios::binary);
        for (int i = 0; i < 200000; ++i) f.put(static_cast<char>((i * 7 + 3) & 0xFF));
    }
    const auto h = movie_hash(path);
    REQUIRE(h.has_value());
    CHECK_EQ(*h, std::string("60a0df1f5fa2cd40"));
    {
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        f << std::string(1000, 'x');  // under 128 KiB: no hash
    }
    CHECK(!movie_hash(path).has_value());
    std::remove(path.c_str());
    CHECK(!movie_hash("/nonexistent/file").has_value());
}

TEST("search_url") {
    CHECK_EQ(search_url("The Film (2020)", "60a0df1f5fa2cd40", "EN"),
             std::string("https://api.opensubtitles.com/api/v1/subtitles?languages=en&moviehash=60a0df1f5fa2cd40&query=the%20film%20%282020%29"));
    CHECK_EQ(search_url("film", "", "fr"), std::string("https://api.opensubtitles.com/api/v1/subtitles?languages=fr&query=film"));
}

TEST("parse_search") {
    const std::string body = R"({"total_count":3,"data":[
      {"id":"1","type":"subtitle","attributes":{"language":"en","release":"Film.2020.720p","download_count":50,
        "hearing_impaired":false,"moviehash_match":false,"files":[{"file_id":111,"file_name":"a.srt"}],
        "feature_details":{"title":"Film","year":2020}}},
      {"id":"2","type":"subtitle","attributes":{"language":"en","release":"Film.2020.1080p","download_count":10,
        "moviehash_match":true,"files":[{"file_id":222,"file_name":"b.srt"}]}},
      {"id":"3","type":"subtitle","attributes":{"language":"en","release":"no files","files":[]}},
      {"id":"4","type":"subtitle","attributes":{"language":"en","release":"Film.SDH","download_count":90,
        "hearing_impaired":true,"files":[{"file_id":333,"file_name":"c.srt"}]}}]})";
    std::vector<Result> r;
    std::string err;
    REQUIRE(parse_search(body, &r, &err));
    REQUIRE(r.size() == 3);  // the one without files left out
    CHECK(r[0].file_id == 222);  // the hash match first
    CHECK(r[0].hash_match);
    CHECK(r[1].file_id == 333);  // then the most downloaded
    CHECK(r[1].hearing_impaired);
    CHECK(r[2].file_id == 111);
    CHECK_EQ(r[2].title, std::string("Film"));
    CHECK(!parse_search("<html>", &r, &err));
}

TEST("parse_download_and_login") {
    Download d;
    std::string err;
    REQUIRE(parse_download(R"({"link":"https://dl/x.srt","file_name":"x.srt","remaining":4,"reset_time":"23 hours"})", &d, &err));
    CHECK_EQ(d.link, std::string("https://dl/x.srt"));
    CHECK(d.remaining == 4);
    CHECK(!parse_download(R"({"message":"You have downloaded your allowed 5 subtitles"})", &d, &err));
    CHECK_EQ(err, std::string("You have downloaded your allowed 5 subtitles"));
    std::string token;
    CHECK(parse_login(R"({"user":{"allowed_downloads":20},"token":"abc.def.ghi","status":200})", &token, &err));
    CHECK_EQ(token, std::string("abc.def.ghi"));
    CHECK(!parse_login("{}", &token, &err));
}

TEST("error_text_and_account") {
    CHECK(error_text(406, R"({"message":"limit"})").find("used up") != std::string::npos);
    CHECK(error_text(401, "").find("sign-in") != std::string::npos);
    CHECK(error_text(429, "").find("too many") != std::string::npos);
    CHECK_EQ(error_text(500, R"({"message":"Server error"})"), std::string("Server error"));
    Account a{"key", "me", "pw"};
    const Account b = account_from_json(account_json(a));
    CHECK(b.configured());
    CHECK_EQ(b.username, std::string("me"));
    CHECK(!account_from_json("nope").configured());
}

namespace {
// Answers requests in order; records them.
struct FakeServer : wnk::net::HttpTransport {
    std::deque<std::pair<long, std::string>> answers;
    std::vector<wnk::net::HttpRequest> seen;
    std::string describe() const override { return "fake"; }
    wnk::net::HttpResponse perform(const wnk::net::HttpRequest &req, const wnk::async::CancellationToken &) override {
        seen.push_back(req);
        wnk::net::HttpResponse r;
        if (answers.empty()) {
            r.error = wnk::net::NetError::Connect;
            return r;
        }
        r.status = answers.front().first;
        r.body = answers.front().second;
        answers.pop_front();
        return r;
    }
};
std::string header(const wnk::net::HttpRequest &r, const std::string &name) {
    for (const auto &h : r.headers)
        if (h.first == name) return h.second;
    return {};
}
}  // namespace

TEST("client_signs_in_again_once_after_401") {
    auto server = std::make_shared<FakeServer>();
    server->answers = {{200, R"({"token":"t1"})"},  // login
                       {401, R"({"message":"expired"})"},  // download: the token expired
                       {200, R"({"token":"t2"})"},  // login again
                       {200, R"({"link":"https://dl/f.srt","file_name":"f.srt","remaining":3})"},
                       {200, "1\n00:00:01,000 --> 00:00:02,000\nHi\n"}};
    Client c(server, Account{"KEY", "me", "pw"});
    std::string text, err;
    Download info;
    REQUIRE(c.download(42, &text, &info, &err, {}));
    CHECK(text.find("Hi") != std::string::npos);
    CHECK(info.remaining == 3);
    REQUIRE(server->seen.size() == 5);
    CHECK_EQ(server->seen[0].url, std::string("https://api.opensubtitles.com/api/v1/login"));
    CHECK_EQ(header(server->seen[0], "Api-Key"), std::string("KEY"));
    CHECK_EQ(header(server->seen[0], "User-Agent"), std::string("Pylon v0.1"));
    CHECK_EQ(header(server->seen[1], "Authorization"), std::string("Bearer t1"));
    CHECK(server->seen[1].body.find("\"file_id\":42") != std::string::npos);
    CHECK_EQ(header(server->seen[3], "Authorization"), std::string("Bearer t2"));
    CHECK_EQ(server->seen[4].url, std::string("https://dl/f.srt"));
    CHECK(header(server->seen[4], "Api-Key").empty());  // the file link gets no API key
}

TEST("client_reports_quota") {
    auto server = std::make_shared<FakeServer>();
    server->answers = {{200, R"({"token":"t1"})"}, {406, R"({"message":"You have downloaded your allowed 5 subtitles"})"}};
    Client c(server, Account{"KEY", "me", "pw"});
    std::string text, err;
    Download info;
    CHECK(!c.download(42, &text, &info, &err, {}));
    CHECK(err.find("used up") != std::string::npos);
}

// Against the real server, only when VPLAYER_TEST_OPENSUBTITLES=1: a search with a made-up key is
// refused, and the refusal is told as such (the requests reach the API as it expects them).
TEST("live_search_with_a_made_up_key") {
    const char *on = std::getenv("VPLAYER_TEST_OPENSUBTITLES");
    if (on == nullptr || std::string(on) != "1") return;
    std::string why;
    std::shared_ptr<wnk::net::HttpTransport> http = wnk::net::make_curl_transport({}, &why);
    REQUIRE(http != nullptr);
    Client c(http, Account{"vplayer-made-up-key", "nobody", "nothing"});
    std::vector<Result> r;
    std::string err;
    CHECK(!c.search("big buck bunny", "", "en", &r, &err, {}));
    std::printf("live search: %s\n", err.c_str());
    CHECK(!err.empty());
}
