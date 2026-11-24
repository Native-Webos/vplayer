// SPDX-License-Identifier: GPL-3.0-or-later
//
// Draws the newest frame of a FrameSink with OpenGL ES 2: the three I420 planes as luminance
// textures, converted to RGB in a shader (BT.709 for HD, BT.601 for SD, limited range), letterboxed
// to the picture's display aspect ratio. Black until the first frame.
#pragma once

#include "player/playback.hpp"

#include "wnk/ui/widgets.hpp"

#include <memory>

namespace vplayer {

// How the picture fills the screen. Fit keeps the whole picture (letterbox); Fill crops it to cover
// the screen; the ratios force a display aspect; Stretch ignores it.
enum class AspectMode : int { Fit = 0, Fill, R16x9, R4x3, R21x9, Stretch, Count };
const char *aspect_name(AspectMode m);

class VideoView : public wnk::ui::GlView {
 public:
    explicit VideoView(std::shared_ptr<FrameSink> frames);
    void set_sink(std::shared_ptr<FrameSink> frames);  // the next file's frames
    void set_aspect(AspectMode m) { aspect_ = m; invalidate_content(); }
    void set_zoom(float z) { zoom_ = z; invalidate_content(); }  // 1 = none
    ~VideoView() override;
    bool has_frame() const { return frames_shown_ > 0; }
    // Call every UI update: schedules a redraw when the sink has a frame newer than the one shown.
    // The view is not "animated": the screen is drawn at the video's frame rate, not the display's.
    void poll();

 protected:
    void render_gl(const wnk::render::CustomTarget &target, double now_ms) override;

 private:
    bool ensure_program();
    std::shared_ptr<FrameSink> sink_;
    unsigned program_ = 0, tex_[3] = {0, 0, 0};
    int tex_w_[3] = {0, 0, 0}, tex_h_[3] = {0, 0, 0};
    int loc_matrix_ = -1, loc_offset_ = -1, loc_scale_ = -1;
    FrameSink::Format fmt_;
    uint64_t serial_ = 0;
    uint64_t frames_shown_ = 0;
    uint64_t polled_ = 0;
    AspectMode aspect_ = AspectMode::Fit;
    float zoom_ = 1;
};

}  // namespace vplayer
