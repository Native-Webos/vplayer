/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * wnk_bridge: a VLC 3 stream output that hands elementary streams to the app (see wnk_bridge.h).
 * Built out of tree against the staged libVLC (libvlc/inside/modules.sh) and installed as
 * lib/vlc/plugins/stream_out/libwnk_bridge_plugin.so.
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <inttypes.h>
#include <stdlib.h>

#include <vlc_common.h>
#include <vlc_plugin.h>
#include <vlc_sout.h>
#include <vlc_block.h>

#include "wnk_bridge.h"

#define SOUT_CFG_PREFIX "sout-wnk_bridge-"

static int Open(vlc_object_t *);
static void Close(vlc_object_t *);

vlc_module_begin()
    set_shortname("wnk bridge")
    set_description("Elementary streams to the app (webOS hardware decoding)")
    set_capability("sout stream", 0)
    add_shortcut("wnk_bridge")
    set_category(CAT_SOUT)
    set_subcategory(SUBCAT_SOUT_STREAM)
    add_string(SOUT_CFG_PREFIX "sink", "0", "Sink", "Address of the app's wnk_bridge_sink", true)
    set_callbacks(Open, Close)
vlc_module_end()

static const char *const ppsz_sout_options[] = {"sink", NULL};

/* ---- VLC enums → ITU-T H.273 code points ---- */

static int h273_primaries(video_color_primaries_t p)
{
    switch (p) {
        case COLOR_PRIMARIES_BT601_525: return 6;
        case COLOR_PRIMARIES_BT601_625: return 5;
        case COLOR_PRIMARIES_BT709:     return 1;
        case COLOR_PRIMARIES_BT2020:    return 9;
        case COLOR_PRIMARIES_DCI_P3:    return 11;
        case COLOR_PRIMARIES_FCC1953:   return 4;
        default:                        return 2;
    }
}

static int h273_transfer(video_transfer_func_t t)
{
    switch (t) {
        case TRANSFER_FUNC_LINEAR:       return 8;
        case TRANSFER_FUNC_SRGB:         return 13;
        case TRANSFER_FUNC_BT470_BG:     return 5;
        case TRANSFER_FUNC_BT470_M:      return 4;
        case TRANSFER_FUNC_BT709:        return 1;
        case TRANSFER_FUNC_SMPTE_ST2084: return 16;
        case TRANSFER_FUNC_SMPTE_240:    return 7;
        case TRANSFER_FUNC_HLG:          return 18;
        default:                         return 2;
    }
}

static int h273_matrix(video_color_space_t s)
{
    switch (s) {
        case COLOR_SPACE_BT601:  return 6;
        case COLOR_SPACE_BT709:  return 1;
        case COLOR_SPACE_BT2020: return 9;
        default:                 return 2;
    }
}

/* VLC 3 timestamps: VLC_TS_INVALID (0) is "none", VLC_TS_0 (1) is time zero. */
static int64_t to_us(mtime_t t)
{
    return t == VLC_TS_INVALID ? WNK_BRIDGE_NO_TIME : (int64_t)(t - VLC_TS_0);
}

/* ---- stream output ---- */

static sout_stream_id_sys_t *Add(sout_stream_t *p_stream, const es_format_t *p_fmt)
{
    wnk_bridge_sink *sink = (wnk_bridge_sink *)p_stream->p_sys;
    wnk_es_desc d;
    memset(&d, 0, sizeof d);
    d.abi = WNK_BRIDGE_ABI;
    switch (p_fmt->i_cat) {
        case VIDEO_ES: d.cat = WNK_ES_VIDEO; break;
        case AUDIO_ES: d.cat = WNK_ES_AUDIO; break;
        case SPU_ES:   d.cat = WNK_ES_SUBTITLE; break;
        default:       return NULL;
    }
    d.es_id = p_fmt->i_id;
    d.codec = p_fmt->i_codec;
    d.original_fourcc = p_fmt->i_original_fourcc;
    d.profile = p_fmt->i_profile;
    d.level = p_fmt->i_level;
    d.bitrate = p_fmt->i_bitrate;
    d.language = p_fmt->psz_language;
    d.extra = (const uint8_t *)p_fmt->p_extra;
    d.extra_size = p_fmt->i_extra > 0 ? (size_t)p_fmt->i_extra : 0;

    if (p_fmt->i_cat == VIDEO_ES) {
        const video_format_t *v = &p_fmt->video;
        d.width = v->i_width;
        d.height = v->i_height;
        d.visible_width = v->i_visible_width;
        d.visible_height = v->i_visible_height;
        d.sar_num = v->i_sar_num;
        d.sar_den = v->i_sar_den;
        d.fps_num = v->i_frame_rate;
        d.fps_den = v->i_frame_rate_base;
        d.primaries = h273_primaries(v->primaries);
        d.transfer = h273_transfer(v->transfer);
        d.matrix = h273_matrix(v->space);
        d.full_range = v->b_color_range_full;
        /* ST 2086 as the HEVC SEI carries it (and VLC stores it): primaries G, B, R in units of
         * 0.00002, luminance in 0.0001 cd/m2. */
        if (v->mastering.max_luminance > 0) {
            static const int order[3] = {2, 0, 1}; /* R from index 2, G from 0, B from 1 */
            d.has_mastering = 1;
            for (int i = 0; i < 3; ++i) {
                d.display_primaries[i][0] = v->mastering.primaries[order[i] * 2] * 0.00002;
                d.display_primaries[i][1] = v->mastering.primaries[order[i] * 2 + 1] * 0.00002;
            }
            d.white_point[0] = v->mastering.white_point[0] * 0.00002;
            d.white_point[1] = v->mastering.white_point[1] * 0.00002;
            d.max_luminance = v->mastering.max_luminance * 0.0001;
            d.min_luminance = v->mastering.min_luminance * 0.0001;
        }
        if (v->lighting.MaxCLL > 0) {
            d.has_light_level = 1;
            d.max_cll = v->lighting.MaxCLL;
            d.max_fall = v->lighting.MaxFALL;
        }
    } else if (p_fmt->i_cat == AUDIO_ES) {
        d.rate = p_fmt->audio.i_rate;
        d.channels = p_fmt->audio.i_channels;
        d.bits_per_sample = p_fmt->audio.i_bitspersample;
    } else {
        d.encoding = p_fmt->subs.psz_encoding;
        d.canvas_width = p_fmt->subs.spu.i_original_frame_width > 0 ? (unsigned)p_fmt->subs.spu.i_original_frame_width : 0;
        d.canvas_height = p_fmt->subs.spu.i_original_frame_height > 0 ? (unsigned)p_fmt->subs.spu.i_original_frame_height : 0;
        d.teletext_magazine = p_fmt->i_codec == VLC_CODEC_TELETEXT ? p_fmt->subs.teletext.i_magazine : -1;
        d.teletext_page = p_fmt->i_codec == VLC_CODEC_TELETEXT ? p_fmt->subs.teletext.i_page : 0;
        if (p_fmt->subs.spu.palette[0] == SPU_PALETTE_DEFINED) {
            d.has_palette = 1;
            memcpy(d.palette, &p_fmt->subs.spu.palette[1], sizeof d.palette);
        }
    }

    void *id = sink->add(sink->opaque, &d);
    msg_Dbg(p_stream, "add %s es %d codec %4.4s: %s", d.cat == WNK_ES_VIDEO ? "video" : d.cat == WNK_ES_AUDIO ? "audio" : "spu",
            d.es_id, (const char *)&d.codec, id ? "taken" : "dropped");
    return (sout_stream_id_sys_t *)id;
}

static void Del(sout_stream_t *p_stream, sout_stream_id_sys_t *id)
{
    wnk_bridge_sink *sink = (wnk_bridge_sink *)p_stream->p_sys;
    sink->del(sink->opaque, id);
}

static int Send(sout_stream_t *p_stream, sout_stream_id_sys_t *id, block_t *p_buffer)
{
    wnk_bridge_sink *sink = (wnk_bridge_sink *)p_stream->p_sys;
    int ret = VLC_SUCCESS;
    while (p_buffer != NULL) {
        block_t *next = p_buffer->p_next;
        p_buffer->p_next = NULL;
        unsigned flags = 0;
        if (p_buffer->i_flags & BLOCK_FLAG_TYPE_I) flags |= WNK_BLOCK_KEY;
        if (p_buffer->i_flags & BLOCK_FLAG_DISCONTINUITY) flags |= WNK_BLOCK_DISCONTINUITY;
        if (p_buffer->i_flags & BLOCK_FLAG_PREROLL) flags |= WNK_BLOCK_PREROLL;
        if (p_buffer->i_flags & BLOCK_FLAG_CORRUPTED) flags |= WNK_BLOCK_CORRUPTED;
        if (ret == VLC_SUCCESS &&
            sink->send(sink->opaque, id, p_buffer->p_buffer, p_buffer->i_buffer, to_us(p_buffer->i_pts),
                       to_us(p_buffer->i_dts), (int64_t)p_buffer->i_length, flags) != 0)
            ret = VLC_EGENERIC;
        block_Release(p_buffer);
        p_buffer = next;
    }
    return ret;
}

static void Flush(sout_stream_t *p_stream, sout_stream_id_sys_t *id)
{
    wnk_bridge_sink *sink = (wnk_bridge_sink *)p_stream->p_sys;
    sink->flush(sink->opaque, id);
}

static int Open(vlc_object_t *p_this)
{
    sout_stream_t *p_stream = (sout_stream_t *)p_this;
    config_ChainParse(p_stream, SOUT_CFG_PREFIX, ppsz_sout_options, p_stream->p_cfg);
    char *address = var_GetString(p_stream, SOUT_CFG_PREFIX "sink");
    wnk_bridge_sink *sink = address ? (wnk_bridge_sink *)(uintptr_t)strtoull(address, NULL, 10) : NULL;
    free(address);
    if (sink == NULL || sink->abi != WNK_BRIDGE_ABI || !sink->add || !sink->del || !sink->send || !sink->flush) {
        msg_Err(p_stream, "no valid sink given (sink=<address of a wnk_bridge_sink>)");
        return VLC_EGENERIC;
    }
    p_stream->p_sys = (sout_stream_sys_t *)sink;
    p_stream->pf_add = Add;
    p_stream->pf_del = Del;
    p_stream->pf_send = Send;
    p_stream->pf_flush = Flush;
    msg_Dbg(p_stream, "wnk_bridge ready");
    return VLC_SUCCESS;
}

static void Close(vlc_object_t *p_this)
{
    (void)p_this; /* the sink belongs to the app */
}
