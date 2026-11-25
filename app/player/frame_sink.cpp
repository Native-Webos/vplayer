// SPDX-License-Identifier: GPL-3.0-or-later
#include "player/playback.hpp"

#include <iterator>
#include <algorithm>

namespace vplayer {

namespace {
int align(int v, int a) { return (v + a - 1) / a * a; }
}  // namespace

const std::vector<std::string> &visual_effect_names() {
    static const std::vector<std::string> names{"Off", "Spectrum", "Oscilloscope", "VU meter", "Spectrometer"};
    return names;
}

const char *visual_effect(int index) {
    static const char *const effects[] = {"", "spectrum", "scope", "vuMeter", "spectrometer"};
    return index > 0 && index < static_cast<int>(std::size(effects)) ? effects[index] : "";
}

std::vector<std::string> equalizer_preset_labels() {
    return {"Flat", "Classical", "Club", "Dance", "Full bass", "Full bass and treble", "Full treble", "Headphones",
            "Large hall", "Live", "Party", "Pop", "Reggae", "Rock", "Ska", "Soft", "Soft rock", "Techno"};
}

void FrameSink::configure(Format &fmt) {
    std::lock_guard<std::mutex> lock(mu_);
    const int cw = (fmt.width + 1) / 2, ch = (fmt.height + 1) / 2;
    // Chroma pitch is exactly half the luma pitch, so the same texture-coordinate crop (width/pitch)
    // fits all three planes when the video view uploads them at their padded width.
    fmt.pitch[0] = align(std::max(fmt.width, 2 * cw), 64);
    fmt.pitch[1] = fmt.pitch[2] = fmt.pitch[0] / 2;
    fmt.lines[0] = align(fmt.height, 16);
    fmt.lines[1] = fmt.lines[2] = fmt.lines[0] / 2 > ch ? fmt.lines[0] / 2 : ch;
    plane_offset_[0] = 0;
    plane_offset_[1] = static_cast<size_t>(fmt.pitch[0]) * fmt.lines[0];
    plane_offset_[2] = plane_offset_[1] + static_cast<size_t>(fmt.pitch[1]) * fmt.lines[1];
    const size_t total = plane_offset_[2] + static_cast<size_t>(fmt.pitch[2]) * fmt.lines[2];
    for (auto &b : buf_) {
        b.assign(total, 0);
        // Black: Y 16 (limited range) or 0 (full range), U/V 128.
        std::fill(b.begin(), b.begin() + static_cast<std::ptrdiff_t>(plane_offset_[1]), uint8_t{fmt.full_range ? 0 : 16});
        std::fill(b.begin() + static_cast<std::ptrdiff_t>(plane_offset_[1]), b.end(), uint8_t{128});
    }
    fmt.visible_width = fmt_.visible_width;
    fmt.visible_height = fmt_.visible_height;
    fmt.sar_num = fmt_.sar_num > 0 ? fmt_.sar_num : 1;
    fmt.sar_den = fmt_.sar_den > 0 ? fmt_.sar_den : 1;
    fmt_ = fmt;
    writing_ = ready_ = reading_ = -1;
    ++generation_;
}

int FrameSink::begin_write(uint8_t *planes[3]) {
    std::lock_guard<std::mutex> lock(mu_);
    int b = 0;
    while (b == ready_ || b == reading_) ++b;  // three buffers: one is always free
    writing_ = b;
    for (int i = 0; i < 3; ++i) planes[i] = buf_[b].data() + plane_offset_[i];
    return b;
}

void FrameSink::end_write(int buffer) {
    std::lock_guard<std::mutex> lock(mu_);
    if (buffer != writing_) return;  // reconfigured meanwhile
    ready_ = buffer;
    writing_ = -1;
    ++serial_;
}

void FrameSink::set_sar(int num, int den) {
    std::lock_guard<std::mutex> lock(mu_);
    if (num > 0 && den > 0) {
        fmt_.sar_num = num;
        fmt_.sar_den = den;
    }
}

void FrameSink::set_visible_size(int width, int height) {
    std::lock_guard<std::mutex> lock(mu_);
    fmt_.visible_width = width;
    fmt_.visible_height = height;
}

bool FrameSink::take(Format *fmt, const uint8_t *planes[3], uint64_t *serial) {
    std::lock_guard<std::mutex> lock(mu_);
    if (ready_ < 0) return false;
    reading_ = ready_;
    ready_ = -1;
    *fmt = fmt_;
    for (int i = 0; i < 3; ++i) planes[i] = buf_[reading_].data() + plane_offset_[i];
    *serial = serial_;
    return true;
}

uint64_t FrameSink::serial() const {
    std::lock_guard<std::mutex> lock(mu_);
    return serial_;
}

FrameSink::Format FrameSink::format() const {
    std::lock_guard<std::mutex> lock(mu_);
    return fmt_;
}

}  // namespace vplayer
