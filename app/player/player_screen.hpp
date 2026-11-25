// SPDX-License-Identifier: GPL-3.0-or-later
//
// The full-screen player: the video, a title line, a floating control bar, and a side panel with
// VLC's controls (mockups "3 · Player controls" and "4 · Audio, subtitles, speed").
//
//   Bar:   previous / next file in the folder · −10 s · play/pause · +10 s ·
//          Audio · Subtitles · speed · Picture · More          plus chapter name and "Ends at"
//   Panel: Audio     track, delay, equalizer, volume boost
//          Subtitles track (or off), delay, load a subtitle file from the folder
//          Video     aspect ratio, zoom, deinterlace, picture adjustments, video track
//          Playback  speed, chapters, A-B repeat, repeat, play next automatically, sleep timer
//          Info      codecs, sizes, bitrates, frames shown and dropped
//
// Remote, controls hidden: Left/Right skip 10 s, OK pauses, Up/Down show the controls.
// Any time: Play/Pause, Rewind/FastForward (30 s), Channel +/- (next/previous file), colour keys
// (red: info, green: speed, yellow: subtitles, blue: audio), Stop/Back leave (Back closes the panel
// first).
#pragma once

#include "pages.hpp"
#include "player/playback.hpp"
#include "player/video_view.hpp"

#include "wnk/app/wake_lock.hpp"
#include "wnk/media/subtitle_view.hpp"
#include "wnk/platform/files.hpp"
#include "wnk/ui/navigation.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace vplayer {

class SeekBar;
class PlayPauseButton;
class PlayerPanel;
class PillButton;

// Settings that stay for the whole player session (they carry over to the next file).
struct PlayerSettings {
    int64_t audio_delay_ms = 0, subtitle_delay_ms = 0;  // reset per file
    int equalizer = -1;          // preset index, AudioEffects::kCustomEqualizer, -1 off
    std::array<float, 11> eq_bands{};  // the custom equalizer: preamp, then the 10 bands (dB)
    int volume = 100;            // percent, up to 200
    bool normalize = false;      // steadier loudness (VLC's normvol)
    bool widen = false;          // wider stereo (VLC's stereo_widen)
    AudioEffects effects() const {
        AudioEffects fx;
        fx.equalizer = equalizer;
        fx.bands = eq_bands;
        fx.volume = volume;
        fx.normalize = normalize;
        fx.widen = widen;
        return fx;
    }
    AspectMode aspect = AspectMode::Fit;
    int zoom = 100;              // percent
    int deinterlace = 0;         // index into kDeinterlace
    PictureAdjust adjust;
    int speed = 3;               // index into kSpeeds (1×)
    int repeat = 0;              // 0 off, 1 this file, 2 folder
    bool autoplay = true;        // next file in the folder when one ends
    int sleep = 0;               // index into the sleep choices
    float sleep_left_s = 0;      // counting down when > 0
    int64_t a_ms = -1, b_ms = -1;  // A-B repeat
};

class PlayerScreen : public wnk::ui::Screen {
 public:
    // Opens a file: the hardware path unless `software` (the TV refused it, or software is forced).
    using Opener = std::function<std::unique_ptr<Playback>(const wnk::platform::FileEntry &, bool software,
                                                             const AudioEffects &effects)>;

    // `queue` is the folder's playable files and `index` the one to start with.
    PlayerScreen(Services &services, wnk::ui::Navigator &nav, Opener open, std::vector<wnk::platform::FileEntry> queue,
                 size_t index);
    ~PlayerScreen() override;

    void on_show(wnk::ui::Ui &ui) override;
    void on_hide(wnk::ui::Ui &ui) override;
    bool on_back(wnk::ui::Ui &ui) override;
    bool on_action(wnk::ui::Ui &ui, const wnk::input::ActionEvent &e) override;
    void update(wnk::ui::Ui &ui, float dt) override;

    // ---- used by the controls and the panel ----
    Playback *playback() { return playback_.get(); }
    PlayerSettings &settings() { return settings_; }
    VideoView &video() { return *video_; }
    const wnk::platform::FileEntry &current() const { return queue_[index_]; }
    void toggle_pause(wnk::ui::Ui &ui);
    void skip(wnk::ui::Ui &ui, int64_t delta_ms);
    void go_to(wnk::ui::Ui &ui, int delta);  // next (+1) / previous (-1) file
    bool has_next() const { return index_ + 1 < queue_.size(); }
    bool has_previous() const { return index_ > 0; }
    void open_panel(wnk::ui::Ui &ui, int tab);
    void close_panel(wnk::ui::Ui &ui);
    void apply_speed();
    // The audio effects changed: applied live where the engine can, else the file reopens here.
    void apply_effects(wnk::ui::Ui &ui);
    void reopen_for_effects(wnk::ui::Ui &ui);
    void find_sidecars(const std::string &path);
    // "Load a subtitle file": the subtitle files in the playing file's folder (a share's too).
    void offer_subtitle_files(wnk::ui::Ui &ui);
    // "Download subtitles": OpenSubtitles with the viewer's account (Settings), the language chosen.
    void download_subtitles(wnk::ui::Ui &ui);
    void search_subtitles(wnk::ui::Ui &ui, const std::string &language);
    void fetch_subtitle(wnk::ui::Ui &ui, const opensubs::Result &result);
    void restart_file(wnk::ui::Ui &ui);  // to 0:00, resume point dropped
    void take_snapshot(wnk::ui::Ui &ui);  // a PNG on the first writable drive
    PlayHistory *history() { return s_.history; }
    std::vector<MyPlaylist> *my_playlists() { return s_.my_playlists; }
    void save_my_playlists() {
        if (s_.save_my_playlists) s_.save_my_playlists();
    }
    wnk::async::WorkerPool &pool() { return s_.ctx->pool(); }
    void refresh_bar();
    // Stops playback (on a worker) and leaves the player.
    void close(wnk::ui::Ui &ui);
    // Opens the panel on `tab` (0 audio … 4 info) once the file plays: launch parameter "panel".
    void open_panel_when_playing(int tab) { pending_panel_ = tab; }
    // Starts the first file at `ms` once it plays (launch parameter "at"; later: resume points).
    void start_at(int64_t ms) { start_at_ms_ = ms; }

 protected:
    void on_descendant_focused(wnk::ui::Ui &ui, View *child, View *focused) override;

 private:
    void start(wnk::ui::Ui &ui);           // opens queue_[index_]
    void stop_current();                   // releases the playback on a worker
    void apply_session();                  // settings that carry over, once the file plays
    void show_controls(wnk::ui::Ui &ui, bool focus_play);
    void hide_controls(wnk::ui::Ui &ui);
    void ended(wnk::ui::Ui &ui);
    void remember_position();              // resume point and history, from the current playback
    void update_subtitles(float dt);             // hardware path: styled ASS, the picture's rectangle

    Services &s_;
    wnk::ui::Navigator &nav_;
    Opener open_;
    std::vector<wnk::platform::FileEntry> queue_;
    size_t index_;
    std::unique_ptr<Playback> playback_;
    PlayerSettings settings_;
    wnk::app::WakeLock wake_;

    VideoView *video_ = nullptr;
    wnk::ui::ImageView *art_ = nullptr;  // audio: the album's cover
    // Hardware path: subtitles drawn by the app over the TV's picture (the software path has VLC
    // blend them into its frames).
    wnk::media::SubtitleView *subs_ = nullptr;
    int subs_track_ = -2;          // the track styled ASS was looked at for
    bool subs_ass_done_ = false;   // its header was found and given to libass (or it has none)
    float subs_retry_s_ = 0;
    std::pair<int, int> subs_size_{0, 0};
    View *catcher_ = nullptr;   // takes focus while the controls are hidden (no visible ring)
    View *overlay_ = nullptr;
    PlayerPanel *panel_ = nullptr;
    View *panel_opener_ = nullptr;  // focus returns here when the panel closes
    wnk::ui::Label *title_ = nullptr, *subtitle_ = nullptr;
    wnk::ui::Label *time_ = nullptr, *length_ = nullptr, *format_ = nullptr;
    wnk::ui::Label *chapter_ = nullptr, *ends_ = nullptr;
    wnk::ui::Spinner *spinner_ = nullptr;
    SeekBar *seek_ = nullptr;
    PlayPauseButton *play_ = nullptr;
    PillButton *speed_btn_ = nullptr;
    View *prev_btn_ = nullptr, *next_btn_ = nullptr;
    float idle_s_ = 0;
    float overlay_alpha_ = 1, overlay_target_ = 1;
    float clock_s_ = 0;
    float speed_settle_s_ = 0;
    float effects_wait_s_ = 0;  // > 0: audio effects to reopen with once it runs out
    float history_s_ = 0;       // since the position was last remembered
    bool ended_ = false;        // this file played to its end
    bool resume_note_ = false;  // start_at_ms_ is a resume point: say so when it applies  // after a speed change, before checking what the engine plays
    bool controls_ = true;
    bool in_menu_ = false;      // a disc menu shows: arrows and OK go to it while the controls are hidden
    float menu_check_s_ = 0;
    bool menu_skipped_ = false;  // disc menus off (Settings): the main film was started once
    bool closing_ = false;
    bool applied_ = false;     // apply_session ran for this file
    // The same file opened again (new audio effects): what the viewer had chosen, restored once the
    // tracks exist.
    struct Reopen {
        int audio_track = -1, subtitle_track = -1;
        int audio_delay_ms = 0, subtitle_delay_ms = 0;
    };
    std::optional<Reopen> reopen_;
    uint64_t opened_ = 0;               // counts start(): a sidecar search applies to its own file only
    wnk::async::Lifetime lifetime_;     // the sidecar search's result is dropped once the screen is gone
    bool software_ = false;    // this file fell back to the software path
    bool reported_ = false;
    int pending_panel_ = -1;
    int64_t start_at_ms_ = -1;
};

}  // namespace vplayer
