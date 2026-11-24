// SPDX-License-Identifier: GPL-3.0-or-later
//
// What was played and where it stopped: resume points and "Continue watching".
//
//   record()          the player reports a file's position (every few seconds and when it leaves
//                     it); near the end (the last 5 % or 2 minutes) the file counts as watched and
//                     its resume point is dropped
//   resume_ms()       where to start a file again: only past the first 30 s, and not when watched
//   continue_list()   unfinished files with a resume point, most recent first
//
// One entry per path (a local path or a libVLC address), newest first, at most 200. Kept in the
// app's key/value store as JSON; loaded once, saved on a worker. Main thread only.
#pragma once

#include "wnk/async/async.hpp"
#include "wnk/storage/kv.hpp"

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace vplayer {

struct HistoryEntry {
    std::string path;      // local path or libVLC address
    std::string name;      // file name as shown
    int64_t position_ms = 0;
    int64_t length_ms = 0;
    int64_t played_at_s = 0;  // seconds since the epoch
    bool watched = false;
};

// ---- pure (tested on the host) ----
constexpr size_t kHistoryMax = 200;
constexpr int64_t kResumeMinMs = 30'000;
// Whether `position` of `length` counts as having watched the file.
bool is_watched(int64_t position_ms, int64_t length_ms);
// Puts `e` first (replacing the entry of the same path); keeps at most kHistoryMax.
void history_record(std::vector<HistoryEntry> *list, HistoryEntry e);
int64_t history_resume_ms(const std::vector<HistoryEntry> &list, const std::string &path);
std::vector<HistoryEntry> history_continue(const std::vector<HistoryEntry> &list, size_t max);
std::string history_json(const std::vector<HistoryEntry> &list);
std::vector<HistoryEntry> history_from_json(const std::string &json);

// Blocking read of the stored history (a worker).
std::vector<HistoryEntry> read_history(wnk::storage::KeyValueStore *store);

// ---- the app's history ----
class PlayHistory {
 public:
    // Blocking load (call at start, from a worker or before the UI needs it).
    void load(wnk::storage::KeyValueStore *store);
    // Takes a list read elsewhere (read_history on a worker), then saves to `store`.
    void adopt(wnk::storage::KeyValueStore *store, std::vector<HistoryEntry> list);
    bool loaded() const { return loaded_; }
    void record(const HistoryEntry &e, wnk::async::WorkerPool &pool);
    void forget(const std::string &path, wnk::async::WorkerPool &pool);
    void restart(const std::string &path, wnk::async::WorkerPool &pool);  // resume point dropped
    int64_t resume_ms(const std::string &path) const { return resume_ ? history_resume_ms(list_, path) : 0; }
    void set_resume(bool on) { resume_ = on; }  // Settings: "Continue where I stopped"
    std::vector<HistoryEntry> continue_list(size_t max) const { return history_continue(list_, max); }
    // Bumps on every change (pages redraw their rail when it moves).
    uint64_t generation() const { return generation_; }

 private:
    void save(wnk::async::WorkerPool &pool);

    struct SaveState {
        std::mutex mu;
        uint64_t written = 0;  // generation last stored
    };
    std::shared_ptr<SaveState> save_state_ = std::make_shared<SaveState>();
    wnk::storage::KeyValueStore *store_ = nullptr;
    std::vector<HistoryEntry> list_;
    bool loaded_ = false;
    bool resume_ = true;
    uint64_t generation_ = 0;
};

}  // namespace vplayer
