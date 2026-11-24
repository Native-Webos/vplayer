// SPDX-License-Identifier: GPL-3.0-or-later
//
// Bitmap subtitles as libVLC's demuxers hand them out (through the wnk_bridge stream output),
// decoded into webos-native-app-kit cues for the hardware path, where VLC does not draw them.
//
//   'bdpg'  Blu-ray PGS (HDMV presentation graphics): segments PCS, WDS, PDS, ODS, END. Palettes
//           and objects carry over between display sets of an epoch; a composition with no object
//           clears the screen. The canvas is the composition's video size.
//   'spu '  DVD subpictures (VobSub in Matroska, DVD/VOB): a 2-bit RLE picture in two fields,
//           coloured through the stream's 16-colour palette (from the bridge's description), with
//           display start and stop in its control sequence (no stop, or 0xFFFF: until the next
//           subpicture). The canvas is the stream's original frame, else the video's size.
//   'dvbs'  DVB subtitles (ETSI EN 300 743, from TV recordings): page and region compositions,
//           colour tables and object data (2-, 4- and 8-bit pixel strings with the standard map
//           tables), drawn into their regions at the end of a display set; a page without regions
//           clears. The display definition gives the canvas (720x576 without one); the page time-out
//           ends the cue.
//
// Colours: YCbCr to RGB, BT.709 for HD canvases, BT.601 otherwise, limited range. Bitmaps are
// cropped to their visible pixels. Pure: no libVLC, testable on the host.
#pragma once

#include "wnk/media/media.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <vector>

namespace vplayer {

class BitmapSubtitleDecoder {
 public:
    using Cue = std::shared_ptr<const wnk::media::SubtitleCue>;

    // `dvd_palette`: 16 entries as VLC holds them (Y << 16 | Cb << 8 | Cr), or nullptr.
    // `canvas_w`/`canvas_h`: the frame the subtitles were authored on, 0 if unknown.
    BitmapSubtitleDecoder(uint32_t vlc_fourcc, const uint32_t *dvd_palette, int canvas_w, int canvas_h);

    static bool handles(uint32_t vlc_fourcc);

    // Video size, for DVD subtitles whose stream does not say what frame they were made for.
    void set_video_size(int width, int height);

    // One packet → the cues it completes (none, or one; a clear cue when the screen empties).
    std::vector<Cue> decode(const uint8_t *data, size_t size, int track, int64_t pts_us);

 private:
    struct PgsObject {
        int width = 0, height = 0;
        size_t expected = 0;        // RLE bytes announced by the first fragment
        std::vector<uint8_t> rle;
    };
    struct PgsPlacement {
        int object = 0, x = 0, y = 0;
        bool cropped = false;
        int crop_x = 0, crop_y = 0, crop_w = 0, crop_h = 0;
    };

    std::vector<Cue> decode_pgs(const uint8_t *data, size_t size, int track, int64_t pts_us);
    std::vector<Cue> decode_dvd(const uint8_t *data, size_t size, int track, int64_t pts_us);
    std::vector<Cue> decode_dvb(const uint8_t *data, size_t size, int track, int64_t pts_us);
    Cue dvb_cue(int track, int64_t pts_us);

    // DVB state: regions, colour tables and objects of the current page.
    struct DvbRegion {
        int width = 0, height = 0, depth = 8;  // depth in bits per pixel: 2, 4 or 8
        int clut = 0;
        bool fill = false;
        int background = 0;
        struct Ref {
            int object = 0, x = 0, y = 0;
        };
        std::vector<Ref> objects;
    };
    struct DvbClut {
        std::array<uint32_t, 4> t2{};
        std::array<uint32_t, 16> t4{};
        std::array<uint32_t, 256> t8{};
    };
    struct DvbObject {
        std::vector<uint8_t> top, bottom;  // pixel-data sub-blocks of each field
    };
    struct DvbPlacement {
        int region = 0, x = 0, y = 0;
    };
    DvbClut default_clut() const;
    void draw_dvb_object(const DvbObject &o, const DvbRegion &r, int ox, int oy, std::vector<uint8_t> *px) const;
    std::map<int, DvbRegion> dvb_regions_;
    std::map<int, DvbClut> dvb_cluts_;
    std::map<int, DvbObject> dvb_objects_;
    std::vector<DvbPlacement> dvb_page_;
    int dvb_timeout_s_ = 0;
    bool dvb_have_page_ = false;
    int dvb_w_ = 720, dvb_h_ = 576;
    int64_t dvb_pts_ = wnk::media::kNoTime;
    uint32_t rgba(int y, int cb, int cr, int a, bool hd) const;

    uint32_t fourcc_;
    bool has_dvd_palette_ = false;
    std::array<uint32_t, 16> dvd_palette_{};
    int canvas_w_ = 0, canvas_h_ = 0;
    int video_w_ = 0, video_h_ = 0;

    // PGS state of the current epoch.
    std::map<int, std::array<uint32_t, 256>> pgs_palettes_;  // id → RGBA (straight alpha)
    std::map<int, PgsObject> pgs_objects_;
    int pgs_w_ = 0, pgs_h_ = 0, pgs_palette_ = 0;
    std::vector<PgsPlacement> pgs_placements_;
    bool pgs_have_pcs_ = false;
    int64_t pgs_pts_ = wnk::media::kNoTime;
};

}  // namespace vplayer
