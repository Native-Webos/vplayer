// SPDX-License-Identifier: GPL-3.0-or-later
#include "history.hpp"

#include "wnk/log.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>

namespace vplayer {

namespace {
constexpr const char *kCat = "history";
constexpr const char *kKey = "play.history";
}  // namespace

bool is_watched(int64_t position_ms, int64_t length_ms) {
    if (length_ms <= 0) return false;
    const int64_t tail = std::min<int64_t>(length_ms / 20, 120'000);  // the last 5 %, at most 2 minutes
    return position_ms >= length_ms - tail;
}

void history_record(std::vector<HistoryEntry> *list, HistoryEntry e) {
    if (e.path.empty()) return;
    e.watched = e.watched || is_watched(e.position_ms, e.length_ms);
    if (e.watched) e.position_ms = 0;
    list->erase(std::remove_if(list->begin(), list->end(), [&](const HistoryEntry &x) { return x.path == e.path; }),
                list->end());
    list->insert(list->begin(), std::move(e));
    if (list->size() > kHistoryMax) list->resize(kHistoryMax);
}

int64_t history_resume_ms(const std::vector<HistoryEntry> &list, const std::string &path) {
    for (const auto &e : list) {
        if (e.path != path) continue;
        if (e.watched || e.position_ms < kResumeMinMs) return 0;
        return e.position_ms;
    }
    return 0;
}

std::vector<HistoryEntry> history_continue(const std::vector<HistoryEntry> &list, size_t max) {
    std::vector<HistoryEntry> out;
    for (const auto &e : list) {
        if (out.size() >= max) break;
        if (!e.watched && e.position_ms >= kResumeMinMs) out.push_back(e);
    }
    return out;
}

std::string history_json(const std::vector<HistoryEntry> &list) {
    nlohmann::json j = nlohmann::json::array();
    for (const auto &e : list)
        j.push_back({{"p", e.path}, {"n", e.name}, {"pos", e.position_ms}, {"len", e.length_ms}, {"at", e.played_at_s},
                     {"w", e.watched}});
    return j.dump();
}

std::vector<HistoryEntry> history_from_json(const std::string &text) {
    std::vector<HistoryEntry> out;
    const auto j = nlohmann::json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_array()) return out;
    for (const auto &x : j) {
        if (!x.is_object() || !x.contains("p") || !x["p"].is_string()) continue;
        HistoryEntry e;
        e.path = x["p"].get<std::string>();
        e.name = x.value("n", std::string());
        e.position_ms = x.value("pos", int64_t{0});
        e.length_ms = x.value("len", int64_t{0});
        e.played_at_s = x.value("at", int64_t{0});
        e.watched = x.value("w", false);
        out.push_back(std::move(e));
        if (out.size() >= kHistoryMax) break;
    }
    return out;
}

std::vector<HistoryEntry> read_history(wnk::storage::KeyValueStore *store) {
    if (store == nullptr) return {};
    wnk::storage::Entry e;
    if (store->get(kKey, &e) != wnk::storage::Status::Ok) return {};
    return history_from_json(e.value);
}

void PlayHistory::load(wnk::storage::KeyValueStore *store) { adopt(store, read_history(store)); }

void PlayHistory::adopt(wnk::storage::KeyValueStore *store, std::vector<HistoryEntry> list) {
    store_ = store;
    loaded_ = true;
    list_ = std::move(list);
    ++generation_;
    WNK_LOGI(kCat, "%zu entries", list_.size());
}

void PlayHistory::record(const HistoryEntry &e, wnk::async::WorkerPool &pool) {
    history_record(&list_, e);
    ++generation_;
    save(pool);
}

void PlayHistory::forget(const std::string &path, wnk::async::WorkerPool &pool) {
    list_.erase(std::remove_if(list_.begin(), list_.end(), [&](const HistoryEntry &x) { return x.path == path; }),
                list_.end());
    ++generation_;
    save(pool);
}

void PlayHistory::restart(const std::string &path, wnk::async::WorkerPool &pool) {
    for (auto &e : list_)
        if (e.path == path) e.position_ms = 0;
    ++generation_;
    save(pool);
}

void PlayHistory::save(wnk::async::WorkerPool &pool) {
    if (store_ == nullptr) return;
    auto *store = store_;
    std::string json = history_json(list_);
    // Saves may run on different workers: the newest generation wins, an older one never overwrites it.
    const uint64_t gen = generation_;
    pool.submit([store, json = std::move(json), gen, state = save_state_](const wnk::async::CancellationToken &) {
        std::lock_guard<std::mutex> lock(state->mu);
        if (gen <= state->written) return;
        if (store->put(kKey, json).status != wnk::storage::Status::Ok) {
            WNK_LOGW(kCat, "history not stored");
            return;
        }
        state->written = gen;
    });
}

}  // namespace vplayer
