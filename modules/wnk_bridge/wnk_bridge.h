/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * wnk_bridge: the contract between the "wnk_bridge" VLC stream output (wnk_bridge.c, a libVLC
 * plugin) and the app that receives its elementary streams.
 *
 * The app plays a media with the sout chain  #wnk_bridge{sink=<address of a wnk_bridge_sink>}
 * (optionally behind #transcode{acodec=...}). VLC demuxes and packetizes; the module hands every
 * elementary stream's description and packets to the sink instead of decoding them, so the TV's own
 * decoder can (webOS: Starfish / NDL through webos-native-app-kit).
 *
 * Plain C, no VLC types: the app does not include VLC's plugin headers.
 *
 * Threading: add/del/send/flush are called from VLC's decoder threads (one per stream). send may
 * block for back-pressure: VLC's input then waits too. A sink must return from send promptly once
 * the app is stopping playback, or libvlc_media_player_stop() waits for it.
 */
#ifndef WNK_BRIDGE_H
#define WNK_BRIDGE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WNK_BRIDGE_ABI 1

/* Timestamps are microseconds of stream time; WNK_BRIDGE_NO_TIME when unknown. */
#define WNK_BRIDGE_NO_TIME INT64_MIN

enum { WNK_ES_VIDEO = 1, WNK_ES_AUDIO = 2, WNK_ES_SUBTITLE = 3 };

enum {
    WNK_BLOCK_KEY = 1,            /* a keyframe (VLC's BLOCK_FLAG_TYPE_I) */
    WNK_BLOCK_DISCONTINUITY = 2,  /* timestamps jump (a seek, a stream change) */
    WNK_BLOCK_PREROLL = 4,        /* decode but do not show (before a seek target) */
    WNK_BLOCK_CORRUPTED = 8
};

typedef struct wnk_es_desc {
    int abi;                      /* WNK_BRIDGE_ABI */
    int cat;                      /* WNK_ES_* */
    int es_id;                    /* VLC's elementary stream id (the track id libVLC reports) */
    uint32_t codec;               /* VLC fourcc, e.g. 'h264', 'hevc', 'mp4a', 'a52 ' */
    uint32_t original_fourcc;
    int profile, level;           /* -1 when unknown */
    uint32_t bitrate;             /* bits per second, 0 when unknown */
    const char *language;         /* may be NULL */
    const uint8_t *extra;         /* codec setup: Annex-B SPS/PPS, AudioSpecificConfig, OpusHead... */
    size_t extra_size;

    /* video */
    unsigned width, height;               /* coded */
    unsigned visible_width, visible_height;
    unsigned sar_num, sar_den;
    unsigned fps_num, fps_den;
    int primaries, transfer, matrix;      /* ITU-T H.273 code points; 2 = unspecified */
    int full_range;
    int has_mastering;                    /* SMPTE ST 2086 */
    double display_primaries[3][2];       /* R, G, B as CIE 1931 (x, y) */
    double white_point[2];
    double max_luminance, min_luminance;  /* cd/m2 */
    int has_light_level;
    unsigned max_cll, max_fall;           /* cd/m2 */

    /* audio */
    unsigned rate, channels, bits_per_sample;
} wnk_es_desc;

typedef struct wnk_bridge_sink {
    int abi;                      /* WNK_BRIDGE_ABI */
    void *opaque;
    /* A new stream. Returns the app's handle for it, or NULL to drop the stream. */
    void *(*add)(void *opaque, const wnk_es_desc *desc);
    void (*del)(void *opaque, void *id);
    /* One packet. Returns 0, or -1 to have VLC treat the stream as failed. May block. */
    int (*send)(void *opaque, void *id, const uint8_t *data, size_t size, int64_t pts, int64_t dts,
                int64_t length, unsigned flags);
    /* Drop what is queued for this stream (VLC seeked or changed stream). */
    void (*flush)(void *opaque, void *id);
} wnk_bridge_sink;

#ifdef __cplusplus
}
#endif

#endif /* WNK_BRIDGE_H */
