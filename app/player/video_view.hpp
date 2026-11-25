// SPDX-License-Identifier: GPL-3.0-or-later
//
// Draws the newest frame of a FrameSink with OpenGL ES 2: the three I420 planes as luminance
// textures, converted to RGB in a shader (BT.709 for HD, BT.601 for SD, limited range), letterboxed
// to the picture's display aspect ratio. Black until the first frame.
#pragma once

#include "player/playback.hpp"

#include "wnk/ui/widgets.hpp"

#include <memory>
#include <string>
#include <vector>

namespace vplayer {

// How the picture fills the screen. Fit keeps the whole picture (letterbox); Fill crops it to cover
// the screen; the ratios force a display aspect; Stretch ignores it.
enum class AspectMode : int { Fit = 0, Fill, R16x9, R4x3, R21x9, Stretch, Count };
const char *aspect_name(AspectMode m);
// Crop (VLC's ratios): keep the middle of the picture at this ratio, as for a film with black bars
// in its picture. 0 is "Off".
const std::vector<std::string> &crop_names();
float crop_ratio(int index);  // 0 for Off
// Scale factors that show a picture of display ratio `dar` cropped to `crop` (0: none): the picture
// is drawn at the crop's ratio, then enlarged by these so the cut part falls outside.
void crop_scale(float dar, float crop, float *dar_out, float *grow_x, float *grow_y);

class VideoView : public wnk::ui::GlView {
 public:
    explicit VideoView(std::shared_ptr<FrameSink> frames);
    void set_sink(std::shared_ptr<FrameSink> frames);  // the next file's frames
    void set_aspect(AspectMode m) { aspect_ = m; invalidate_content(); }
    void set_zoom(float z) { zoom_ = z; invalidate_content(); }  // 1 = none
    void set_crop(float ratio) { crop_ = ratio; invalidate_content(); }  // 0 = none
    // Quarter turns clockwise (0..3), then a left-right mirror: drawn by the view (the software path).
    void set_transform(int quarter_turns, bool mirror) {
        turns_ = ((quarter_turns % 4) + 4) % 4;
        mirror_ = mirror;
        invalidate_content();
    }
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
    float crop_ = 0;
    int turns_ = 0;
    bool mirror_ = false;
};

}  // namespace vplayer
