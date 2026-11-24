// SPDX-License-Identifier: GPL-3.0-or-later
#include "player/video_view.hpp"

#include "wnk/log.hpp"

#include <GLES2/gl2.h>

#include <algorithm>

namespace vplayer {

namespace {

constexpr const char *kVertex = R"(
attribute vec2 a_pos;
attribute vec2 a_uv;
varying vec2 v_uv;
uniform vec2 u_scale;  // texture coordinates of the visible picture inside the padded planes
void main() {
    v_uv = a_uv * u_scale;
    gl_Position = vec4(a_pos, 0.0, 1.0);
}
)";

constexpr const char *kFragment = R"(
precision mediump float;
varying vec2 v_uv;
uniform sampler2D u_y, u_u, u_v;
uniform mat3 u_matrix;
uniform vec3 u_offset;
void main() {
    vec3 yuv = vec3(texture2D(u_y, v_uv).r, texture2D(u_u, v_uv).r, texture2D(u_v, v_uv).r) - u_offset;
    gl_FragColor = vec4(clamp(u_matrix * yuv, 0.0, 1.0), 1.0);
}
)";

GLuint compile(GLenum type, const char *src) {
    const GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512] = {};
        glGetShaderInfoLog(s, sizeof log, nullptr, log);
        WNK_LOGE("player", "video shader: %s", log);
    }
    return s;
}

// YUV → RGB, column-major for GL. Rows: R, G, B; columns: Y, U, V. Limited range unless `full`.
void yuv_matrix(bool bt709, bool full, float m[9]) {
    const float ys = full ? 1.0f : 255.0f / 219.0f, cs = full ? 1.0f : 255.0f / 224.0f;
    float kr = 0.299f, kb = 0.114f;
    if (bt709) kr = 0.2126f, kb = 0.0722f;
    const float kg = 1 - kr - kb;
    const float rv = 2 * (1 - kr), bu = 2 * (1 - kb);
    const float gu = -bu * kb / kg, gv = -rv * kr / kg;
    // column 0 (Y), column 1 (U), column 2 (V)
    m[0] = ys, m[1] = ys, m[2] = ys;
    m[3] = 0, m[4] = gu * cs, m[5] = bu * cs;
    m[6] = rv * cs, m[7] = gv * cs, m[8] = 0;
}

}  // namespace

const char *aspect_name(AspectMode m) {
    switch (m) {
        case AspectMode::Fit: return "Fit";
        case AspectMode::Fill: return "Fill screen";
        case AspectMode::R16x9: return "16:9";
        case AspectMode::R4x3: return "4:3";
        case AspectMode::R21x9: return "21:9";
        case AspectMode::Stretch: return "Stretch";
        default: return "";
    }
}

void VideoView::set_sink(std::shared_ptr<FrameSink> frames) {
    sink_ = std::move(frames);
    serial_ = polled_ = 0;
    frames_shown_ = 0;
    fmt_ = {};
    invalidate_content();
}

VideoView::VideoView(std::shared_ptr<FrameSink> frames) : sink_(std::move(frames)) {
    set_animated(false);  // redrawn when poll() sees a new frame (see header)
}

void VideoView::poll() {
    const uint64_t s = sink_ ? sink_->serial() : 0;
    if (s != polled_) {
        polled_ = s;
        invalidate_content();
    }
}

VideoView::~VideoView() {
    // GL objects die with the context when the app ends; a player screen closing mid-run frees its own.
    if (program_ != 0) glDeleteProgram(program_);
    if (tex_[0] != 0) glDeleteTextures(3, tex_);
}

bool VideoView::ensure_program() {
    if (program_ != 0) return true;
    const GLuint vs = compile(GL_VERTEX_SHADER, kVertex), fs = compile(GL_FRAGMENT_SHADER, kFragment);
    program_ = glCreateProgram();
    glAttachShader(program_, vs);
    glAttachShader(program_, fs);
    glBindAttribLocation(program_, 0, "a_pos");
    glBindAttribLocation(program_, 1, "a_uv");
    glLinkProgram(program_);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = 0;
    glGetProgramiv(program_, GL_LINK_STATUS, &ok);
    if (!ok) {
        WNK_LOGE("player", "video shader did not link");
        glDeleteProgram(program_);
        program_ = 0;
        return false;
    }
    glUseProgram(program_);
    glUniform1i(glGetUniformLocation(program_, "u_y"), 0);
    glUniform1i(glGetUniformLocation(program_, "u_u"), 1);
    glUniform1i(glGetUniformLocation(program_, "u_v"), 2);
    loc_matrix_ = glGetUniformLocation(program_, "u_matrix");
    loc_offset_ = glGetUniformLocation(program_, "u_offset");
    loc_scale_ = glGetUniformLocation(program_, "u_scale");
    glGenTextures(3, tex_);
    for (GLuint t : tex_) {
        glBindTexture(GL_TEXTURE_2D, t);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    return true;
}

void VideoView::render_gl(const wnk::render::CustomTarget &t, double) {
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    if (!sink_ || !ensure_program()) return;

    // Upload a newer frame if there is one. Planes are uploaded at their padded pitch (ES 2 has no
    // UNPACK_ROW_LENGTH); u_scale crops the padding away.
    FrameSink::Format f;
    const uint8_t *planes[3];
    uint64_t serial = 0;
    if (sink_->take(&f, planes, &serial) && serial != serial_) {
        serial_ = serial;
        fmt_ = f;
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        for (int i = 0; i < 3; ++i) {
            glActiveTexture(GL_TEXTURE0 + i);
            glBindTexture(GL_TEXTURE_2D, tex_[i]);
            const int w = f.pitch[i], h = i == 0 ? f.height : (f.height + 1) / 2;
            if (w != tex_w_[i] || h != tex_h_[i]) {
                glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, w, h, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, planes[i]);
                tex_w_[i] = w;
                tex_h_[i] = h;
            } else {
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_LUMINANCE, GL_UNSIGNED_BYTE, planes[i]);
            }
        }
        ++frames_shown_;
    }
    if (frames_shown_ == 0 || fmt_.width <= 0 || fmt_.height <= 0) {
        glActiveTexture(GL_TEXTURE0);
        return;
    }

    // The visible picture inside the coded one (libVLC hands over 1280×738 for 1280×720).
    const FrameSink::Format now = sink_->format();  // the visible size can arrive after the first frame
    int vw = now.visible_width > 0 && now.visible_width <= fmt_.width ? now.visible_width : fmt_.width;
    int vh = now.visible_height > 0 && now.visible_height <= fmt_.height ? now.visible_height : fmt_.height;
    const int sar_num = now.sar_num > 0 ? now.sar_num : 1, sar_den = now.sar_den > 0 ? now.sar_den : 1;

    // Fit the display aspect ratio into the view as the aspect mode says, then zoom.
    float dar = static_cast<float>(vw) * sar_num / (static_cast<float>(vh) * sar_den);
    switch (aspect_) {
        case AspectMode::R16x9: dar = 16.0f / 9; break;
        case AspectMode::R4x3: dar = 4.0f / 3; break;
        case AspectMode::R21x9: dar = 21.0f / 9; break;
        default: break;
    }
    const float view = static_cast<float>(t.width) / std::max(1, t.height);
    float sx = 1, sy = 1;
    if (aspect_ == AspectMode::Fill) {
        // Cover the view; the scissor (the view's rectangle) crops what spills over.
        if (dar > view) {
            sx = dar / view;
        } else {
            sy = view / dar;
        }
    } else if (aspect_ != AspectMode::Stretch) {
        if (dar > view) {
            sy = view / dar;
        } else {
            sx = dar / view;
        }
    }
    sx *= zoom_;
    sy *= zoom_;
    const GLfloat quad[] = {
        // x, y, u, v  (v flipped: row 0 of the picture is the top)
        -sx, -sy, 0, 1,  sx, -sy, 1, 1,  -sx, sy, 0, 0,  sx, sy, 1, 0,
    };

    glUseProgram(program_);
    float m[9];
    yuv_matrix(vh >= 720, fmt_.full_range, m);
    glUniformMatrix3fv(loc_matrix_, 1, GL_FALSE, m);
    glUniform3f(loc_offset_, fmt_.full_range ? 0.0f : 16.0f / 255, 128.0f / 255, 128.0f / 255);
    glUniform2f(loc_scale_, static_cast<float>(vw) / fmt_.pitch[0], static_cast<float>(vh) / fmt_.height);
    for (int i = 0; i < 3; ++i) {
        glActiveTexture(GL_TEXTURE0 + i);
        glBindTexture(GL_TEXTURE_2D, tex_[i]);
    }
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glDisable(GL_BLEND);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), quad);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), quad + 2);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisableVertexAttribArray(1);
    glActiveTexture(GL_TEXTURE0);
}

}  // namespace vplayer
