/*
 * Android MediaCodec decoder
 *
 * Copyright (c) 2015-2016 Matthieu Bouron <matthieu.bouron stupeflix.com>
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include <string.h>
#include <stdarg.h>
#include <sys/types.h>

#include "libavutil/avassert.h"
#include "libavutil/common.h"
#include "libavutil/hwcontext_mediacodec.h"
#include "libavutil/mem.h"
#include "libavutil/pixdesc.h"
#include "libavutil/sha.h"
#include "libavutil/log.h"
#include "libavutil/pixfmt.h"
#include "libavutil/time.h"
#include "libavutil/timestamp.h"
#include "libavutil/channel_layout.h"

#include "avcodec.h"
#include "decode.h"
#include "hevc/hevc.h"

#include "mediacodec.h"
#include "mediacodec_surface.h"
#include "mediacodec_sw_buffer.h"
#include "mediacodec_wrapper.h"
#include "mediacodecdec_common.h"

/**
 * OMX.k3.video.decoder.avc, OMX.NVIDIA.* OMX.SEC.avc.dec and OMX.google
 * codec workarounds used in various place are taken from the Gstreamer
 * project.
 *
 * Gstreamer references:
 * https://cgit.freedesktop.org/gstreamer/gst-plugins-bad/tree/sys/androidmedia/
 *
 * Gstreamer copyright notice:
 *
 * Copyright (C) 2012, Collabora Ltd.
 *   Author: Sebastian Dröge <sebastian.droege@collabora.co.uk>
 *
 * Copyright (C) 2012, Rafaël Carré <funman@videolanorg>
 *
 * Copyright (C) 2015, Sebastian Dröge <sebastian@centricular.com>
 *
 * Copyright (C) 2014-2015, Collabora Ltd.
 *   Author: Matthieu Bouron <matthieu.bouron@gcollabora.com>
 *
 * Copyright (C) 2015, Edward Hervey
 *   Author: Edward Hervey <bilboed@gmail.com>
 *
 * Copyright (C) 2015, Matthew Waters <matthew@centricular.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation
 * version 2.1 of the License.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301 USA
 *
 */

/* Only the explicit native-DV diagnostic option calls begin with enabled=1.
 * IDs are process-local monotonic counters, never addresses. */
static atomic_uint_fast64_t native_dv_diag_next_id = ATOMIC_VAR_INIT(0);

/* Reserve event 4096 for a single truncation marker; close/init-failure are
 * separately bounded terminal events. Check before hashing or scanning. */
static void native_dv_diag_limit(AVCodecContext *avctx, MediaCodecNativeDvDiag *d)
{
    if (!d->event_limit_logged) {
        av_log(avctx, AV_LOG_INFO,
               "native_dv_diag decoder=%"PRIu64" event=instance_limit max_events=4096\n",
               d->decoder_id);
        d->event_limit_logged = 1;
        d->event_count++;
    }
    d->enabled = d->capture_au = 0;
}

static int native_dv_diag_allow(AVCodecContext *avctx, MediaCodecNativeDvDiag *d)
{
    if (!d->enabled)
        return 0;
    if (d->event_count >= 4095) {
        native_dv_diag_limit(avctx, d);
        return 0;
    }
    return 1;
}

void ff_mediacodec_diag_log(AVCodecContext *avctx, MediaCodecNativeDvDiag *d,
                           const char *format, ...)
{
    va_list ap;
    if (!native_dv_diag_allow(avctx, d))
        return;
    d->event_count++;
    va_start(ap, format);
    av_vlog(avctx, AV_LOG_INFO, format, ap);
    va_end(ap);
}

static int native_dv_diag_hash(AVCodecContext *avctx, MediaCodecNativeDvDiag *d,
                               const uint8_t *data, size_t size, char hex[65])
{
    struct AVSHA *sha;
    uint8_t digest[32];
    static const char digits[] = "0123456789abcdef";
    int i;

    if (!native_dv_diag_allow(avctx, d))
        return 0;
    sha = av_sha_alloc();
    if (!sha || av_sha_init(sha, 256) < 0 || (size && !data)) {
        av_free(sha);
        ff_mediacodec_diag_log(avctx, d,
               "native_dv_diag decoder=%"PRIu64" event=disabled reason=hash_unavailable\n",
               d->decoder_id);
        d->enabled = d->capture_au = 0;
        return 0;
    }
    if (size)
        av_sha_update(sha, data, size);
    av_sha_final(sha, digest);
    av_free(sha);
    for (i = 0; i < 32; i++) {
        hex[2 * i] = digits[digest[i] >> 4];
        hex[2 * i + 1] = digits[digest[i] & 15];
    }
    hex[64] = 0;
    return 1;
}

void ff_mediacodec_diag_begin(AVCodecContext *avctx, MediaCodecNativeDvDiag *d,
                             int enabled)
{
    if (!enabled)
        return;
    d->requested = d->enabled = 1;
    d->decoder_id = atomic_fetch_add(&native_dv_diag_next_id, 1) + 1;
    ff_mediacodec_diag_log(avctx, d,
           "native_dv_diag decoder=%"PRIu64" epoch=0 event=begin max_epochs=8 max_aus=96 max_nals=16 max_fragments=16 max_events=4096\n",
           d->decoder_id);
}

void ff_mediacodec_diag_blob(AVCodecContext *avctx, MediaCodecNativeDvDiag *d,
                            const char *event, const uint8_t *data, size_t size)
{
    char hex[65];
    if (native_dv_diag_hash(avctx, d, data, size, hex))
        ff_mediacodec_diag_log(avctx, d,
               "native_dv_diag decoder=%"PRIu64" epoch=%d event=%s len=%zu sha256=%s\n",
               d->decoder_id, d->epoch, event, size, hex);
}

/* Annex-B start codes only: do not guess an RPU's meaning or parse its body. */
static int native_dv_diag_start_code(const uint8_t *data, int size, int from,
                                     int *header)
{
    int i;
    for (i = from; i <= size - 3; i++) {
        if (data[i] || data[i + 1])
            continue;
        if (data[i + 2] == 1) {
            *header = i + 3;
            return i;
        }
        if (i <= size - 4 && !data[i + 2] && data[i + 3] == 1) {
            *header = i + 4;
            return i;
        }
    }
    return -1;
}

void ff_mediacodec_diag_au(AVCodecContext *avctx, MediaCodecNativeDvDiag *d,
                          const AVPacket *pkt)
{
    char hex[65];
    char nals[512] = { 0 };
    int header, start, next, next_header, count = 0, malformed = 0;
    int vps = 0, sps = 0, pps = 0, rpu = 0, used = 0;

    if (!native_dv_diag_allow(avctx, d))
        return;
    d->capture_au = 0;
    d->au_seq++;
    if (d->epoch >= 8)
        return;
    if (d->au_count >= 96) {
        if (!d->au_limit_logged) {
            d->au_limit_logged = 1;
            ff_mediacodec_diag_log(avctx, d,
                   "native_dv_diag decoder=%"PRIu64" epoch=%d event=au_limit\n",
                   d->decoder_id, d->epoch);
        }
        return;
    }
    if (pkt->size < 0) {
        ff_mediacodec_diag_log(avctx, d,
               "native_dv_diag decoder=%"PRIu64" event=disabled reason=negative_au_size\n",
               d->decoder_id);
        d->enabled = 0;
        return;
    }
    if (!native_dv_diag_hash(avctx, d, pkt->data, pkt->size, hex))
        return;
    d->au_count++;
    d->au_offset = d->fragment_seq = 0;
    d->au_size = pkt->size;
    d->capture_au = pkt->size > 0;
    start = native_dv_diag_start_code(pkt->data, pkt->size, 0, &header);
    if (start != 0)
        malformed++;
    while (start >= 0) {
        int len, type = -1;
        next_header = 0;
        next = native_dv_diag_start_code(pkt->data, pkt->size, header, &next_header);
        len = (next >= 0 ? next : pkt->size) - header;
        if (len < 2 || (pkt->data[header] & 0x80) || !(pkt->data[header + 1] & 7)) {
            malformed++;
        } else {
            type = (pkt->data[header] >> 1) & 63;
            vps += type == 32;
            sps += type == 33;
            pps += type == 34;
            rpu += type == 62;
        }
        if (count < 16)
            used += snprintf(nals + used, sizeof(nals) - used, "%s%d:%d",
                             count ? "," : "", type, len);
        count++;
        start = next;
        header = next_header;
    }
    ff_mediacodec_diag_log(avctx, d,
           "native_dv_diag decoder=%"PRIu64" epoch=%d event=au au=%"PRIu64
           " pts=%"PRId64" dts=%"PRId64" tb=%d/%d len=%d sha256=%s"
           " nal_count=%d vps=%d sps=%d pps=%d rpu=%d malformed=%d nals=%s\n",
           d->decoder_id, d->epoch, d->au_seq, pkt->pts, pkt->dts,
           avctx->pkt_timebase.num, avctx->pkt_timebase.den, pkt->size, hex,
           count, vps, sps, pps, rpu, malformed, nals);
}

static int native_dv_diag_queue(AVCodecContext *avctx, MediaCodecNativeDvDiag *d,
                                size_t capacity, const uint8_t *data,
                                size_t size, int64_t pts, uint32_t flags, int eos)
{
    char hex[65];
    if (!d->enabled || d->epoch >= 8 ||
        (eos ? d->eos_logged : !d->capture_au))
        return 0;
    /* Reserve both queue-before and queue-result before touching SHA. */
    if (d->event_count >= 4094) {
        native_dv_diag_limit(avctx, d);
        return 0;
    }
    if (!eos && d->fragment_seq >= 16) {
        ff_mediacodec_diag_log(avctx, d,
               "native_dv_diag decoder=%"PRIu64" epoch=%d event=au_fragment_limit au=%"PRIu64" max_fragments=16\n",
               d->decoder_id, d->epoch, d->au_seq);
        d->capture_au = 0;
        return 0;
    }
    if (!eos && (!size || size > d->au_size || d->au_offset > d->au_size - size)) {
        ff_mediacodec_diag_log(avctx, d,
               "native_dv_diag decoder=%"PRIu64" event=disabled reason=au_offset\n",
               d->decoder_id);
        d->enabled = d->capture_au = 0;
        return 0;
    }
    if (!native_dv_diag_hash(avctx, d, data, size, hex))
        return 0;
    if (eos)
        d->eos_logged = 1;
    else
        d->fragment_seq++;
    ff_mediacodec_diag_log(avctx, d,
           "native_dv_diag decoder=%"PRIu64" epoch=%d event=%s au=%"PRIu64
           " fragment=%"PRIu64" au_offset=%"PRIu64" capacity=%zu len=%zu pts_us=%"PRId64
           " flags=%u sha256=%s\n",
           d->decoder_id, d->epoch, eos ? "eos_queue" : "queue",
           eos ? 0 : d->au_seq, eos ? 0 : d->fragment_seq,
           eos ? 0 : d->au_offset, capacity, size, pts, flags, hex);
    return 1;
}

static void native_dv_diag_queue_result(AVCodecContext *avctx, MediaCodecNativeDvDiag *d,
                                       int captured, size_t size, int status, int eos)
{
    if (!captured)
        return;
    ff_mediacodec_diag_log(avctx, d,
           "native_dv_diag decoder=%"PRIu64" epoch=%d event=%s au=%"PRIu64
           " fragment=%"PRIu64" status=%d\n",
           d->decoder_id, d->epoch, eos ? "eos_result" : "queue_result",
           eos ? 0 : d->au_seq, eos ? 0 : d->fragment_seq, status);
    if (!eos && status >= 0) {
        d->au_offset += size;
        if (d->au_offset == d->au_size)
            d->capture_au = 0;
    }
}

static void native_dv_diag_flush(AVCodecContext *avctx, MediaCodecNativeDvDiag *d,
                                int status)
{
    if (!d->enabled || d->epoch >= 8 || (status < 0 && d->flush_failed_logged))
        return;
    if (status < 0)
        d->flush_failed_logged = 1;
    ff_mediacodec_diag_log(avctx, d,
           "native_dv_diag decoder=%"PRIu64" epoch=%d event=flush status=%d\n",
           d->decoder_id, d->epoch, status);
    if (status < 0)
        return;
    d->epoch++;
    d->au_count = d->au_limit_logged = d->capture_au = d->eos_logged = 0;
    d->flush_failed_logged = 0;
    d->au_offset = d->fragment_seq = 0;
    if (d->epoch == 8 && !d->epoch_limit_logged++)
        ff_mediacodec_diag_log(avctx, d,
               "native_dv_diag decoder=%"PRIu64" event=epoch_limit\n", d->decoder_id);
}

void ff_mediacodec_diag_close(AVCodecContext *avctx, MediaCodecNativeDvDiag *d)
{
    if (!d->requested || d->closed)
        return;
    av_log(avctx, AV_LOG_INFO,
           "native_dv_diag decoder=%"PRIu64" epoch=%d event=close enabled=%d\n",
           d->decoder_id, d->epoch, d->enabled);
    d->closed = 1;
    d->enabled = d->capture_au = 0;
}

#define INPUT_DEQUEUE_TIMEOUT_US 8000
#define OUTPUT_DEQUEUE_TIMEOUT_US 8000
#define OUTPUT_DEQUEUE_BLOCK_TIMEOUT_US 1000000

enum {
    ENCODING_PCM_16BIT        = 0x00000002,
    ENCODING_PCM_8BIT         = 0x00000003,
    ENCODING_PCM_FLOAT        = 0x00000004,
    ENCODING_PCM_24BIT_PACKED = 0x00000015,
    ENCODING_PCM_32BIT        = 0x00000016,
};

static const struct {

    int pcm_format;
    enum AVSampleFormat sample_format;

} sample_formats[] = {

    { ENCODING_PCM_16BIT,        AV_SAMPLE_FMT_S16 },
    { ENCODING_PCM_8BIT,         AV_SAMPLE_FMT_U8  },
    { ENCODING_PCM_FLOAT,        AV_SAMPLE_FMT_FLT },
    { ENCODING_PCM_32BIT,        AV_SAMPLE_FMT_S32 },
    { 0 }
};

static enum AVSampleFormat mcdec_map_pcm_format(AVCodecContext *avctx,
                                               MediaCodecDecContext *s,
                                               int pcm_format)
{
    enum AVSampleFormat ret = AV_SAMPLE_FMT_NONE;

    for (int i = 0; i < FF_ARRAY_ELEMS(sample_formats); i++) {
        if (sample_formats[i].pcm_format == pcm_format) {
            return sample_formats[i].sample_format;
        }
    }

    av_log(avctx, AV_LOG_ERROR, "Output sample format 0x%x (value=%d) is not supported\n",
           pcm_format, pcm_format);

    return ret;
}

enum
{
    CHANNEL_OUT_FRONT_LEFT                 = 0x4,
    CHANNEL_OUT_FRONT_RIGHT                = 0x8,
    CHANNEL_OUT_FRONT_CENTER               = 0x10,
    CHANNEL_OUT_LOW_FREQUENCY              = 0x20,
    CHANNEL_OUT_BACK_LEFT                  = 0x40,
    CHANNEL_OUT_BACK_RIGHT                 = 0x80,
    CHANNEL_OUT_FRONT_LEFT_OF_CENTER       = 0x100,
    CHANNEL_OUT_FRONT_RIGHT_OF_CENTER      = 0x200,
    CHANNEL_OUT_BACK_CENTER                = 0x400,
    CHANNEL_OUT_SIDE_LEFT                  = 0x800,
    CHANNEL_OUT_SIDE_RIGHT                 = 0x1000,
    CHANNEL_OUT_TOP_CENTER                 = 0x2000,
    CHANNEL_OUT_TOP_FRONT_LEFT             = 0x4000,
    CHANNEL_OUT_TOP_FRONT_CENTER           = 0x8000,
    CHANNEL_OUT_TOP_FRONT_RIGHT            = 0x10000,
    CHANNEL_OUT_TOP_BACK_LEFT              = 0x20000,
    CHANNEL_OUT_TOP_BACK_CENTER            = 0x40000,
    CHANNEL_OUT_TOP_BACK_RIGHT             = 0x80000,
};

static const struct {

    int mask;
    uint64_t layout;

} channel_masks[] = {
    { CHANNEL_OUT_FRONT_LEFT,            AV_CH_FRONT_LEFT },
    { CHANNEL_OUT_FRONT_RIGHT,           AV_CH_FRONT_RIGHT },
    { CHANNEL_OUT_FRONT_CENTER,          AV_CH_FRONT_CENTER },
    { CHANNEL_OUT_LOW_FREQUENCY,         AV_CH_LOW_FREQUENCY },
    { CHANNEL_OUT_BACK_LEFT,             AV_CH_BACK_LEFT },
    { CHANNEL_OUT_BACK_RIGHT,            AV_CH_BACK_RIGHT },
    { CHANNEL_OUT_FRONT_LEFT_OF_CENTER,  AV_CH_FRONT_LEFT_OF_CENTER },
    { CHANNEL_OUT_FRONT_RIGHT_OF_CENTER, AV_CH_FRONT_RIGHT_OF_CENTER },
    { CHANNEL_OUT_BACK_CENTER,           AV_CH_BACK_CENTER },
    { CHANNEL_OUT_SIDE_LEFT,             AV_CH_SIDE_LEFT },
    { CHANNEL_OUT_SIDE_RIGHT,            AV_CH_SIDE_RIGHT },
    { CHANNEL_OUT_TOP_CENTER,            AV_CH_TOP_CENTER },
    { CHANNEL_OUT_TOP_FRONT_LEFT,        AV_CH_TOP_FRONT_LEFT },
    { CHANNEL_OUT_TOP_FRONT_CENTER,      AV_CH_TOP_FRONT_CENTER },
    { CHANNEL_OUT_TOP_FRONT_RIGHT,       AV_CH_TOP_FRONT_RIGHT },
    { CHANNEL_OUT_TOP_BACK_LEFT,         AV_CH_TOP_BACK_LEFT },
    { CHANNEL_OUT_TOP_BACK_CENTER,       AV_CH_TOP_BACK_CENTER },
    { CHANNEL_OUT_TOP_BACK_RIGHT,        AV_CH_TOP_BACK_RIGHT },
};

static uint64_t mcdec_map_channel_mask(AVCodecContext *avctx,
                                       int channel_mask)
{
    uint64_t channel_layout = 0;

    for (int i = 0; i < FF_ARRAY_ELEMS(channel_masks); i++) {
        if (channel_mask & channel_masks[i].mask)
            channel_layout |= channel_masks[i].layout;
    }

    return channel_layout;
}

enum {
    COLOR_FormatYUV420Planar                              = 0x13,
    COLOR_FormatYUV420SemiPlanar                          = 0x15,
    COLOR_FormatYCbYCr                                    = 0x19,
    COLOR_FormatAndroidOpaque                             = 0x7F000789,
    COLOR_QCOM_FormatYUV420SemiPlanar                     = 0x7fa30c00,
    COLOR_QCOM_FormatYUV420SemiPlanar32m                  = 0x7fa30c04,
    COLOR_QCOM_FormatYUV420PackedSemiPlanar64x32Tile2m8ka = 0x7fa30c03,
    COLOR_TI_FormatYUV420PackedSemiPlanar                 = 0x7f000100,
    COLOR_TI_FormatYUV420PackedSemiPlanarInterlaced       = 0x7f000001,
};

static const struct {

    int color_format;
    enum AVPixelFormat pix_fmt;

} color_formats[] = {

    { COLOR_FormatYUV420Planar,                              AV_PIX_FMT_YUV420P },
    { COLOR_FormatYUV420SemiPlanar,                          AV_PIX_FMT_NV12    },
    { COLOR_QCOM_FormatYUV420SemiPlanar,                     AV_PIX_FMT_NV12    },
    { COLOR_QCOM_FormatYUV420SemiPlanar32m,                  AV_PIX_FMT_NV12    },
    { COLOR_QCOM_FormatYUV420PackedSemiPlanar64x32Tile2m8ka, AV_PIX_FMT_NV12    },
    { COLOR_TI_FormatYUV420PackedSemiPlanar,                 AV_PIX_FMT_NV12    },
    { COLOR_TI_FormatYUV420PackedSemiPlanarInterlaced,       AV_PIX_FMT_NV12    },
    { 0 }
};

static enum AVPixelFormat mcdec_map_color_format(AVCodecContext *avctx,
                                                 MediaCodecDecContext *s,
                                                 int color_format)
{
    int i;
    enum AVPixelFormat ret = AV_PIX_FMT_NONE;

    if (s->surface) {
        return AV_PIX_FMT_MEDIACODEC;
    }

    if (!strcmp(s->codec_name, "OMX.k3.video.decoder.avc") && color_format == COLOR_FormatYCbYCr) {
        s->color_format = color_format = COLOR_TI_FormatYUV420PackedSemiPlanar;
    }

    for (i = 0; i < FF_ARRAY_ELEMS(color_formats); i++) {
        if (color_formats[i].color_format == color_format) {
            return color_formats[i].pix_fmt;
        }
    }

    av_log(avctx, AV_LOG_ERROR, "Output color format 0x%x (value=%d) is not supported\n",
        color_format, color_format);

    return ret;
}

static void ff_mediacodec_dec_ref(MediaCodecDecContext *s)
{
    atomic_fetch_add(&s->refcount, 1);
}

static void ff_mediacodec_dec_unref(MediaCodecDecContext *s)
{
    if (!s)
        return;

    if (atomic_fetch_sub(&s->refcount, 1) == 1) {
        if (s->codec) {
            ff_AMediaCodec_delete(s->codec);
            s->codec = NULL;
        }

        if (s->format) {
            ff_AMediaFormat_delete(s->format);
            s->format = NULL;
        }

        if (s->surface) {
            ff_mediacodec_surface_unref(s->surface, NULL);
            s->surface = NULL;
        }

        av_freep(&s->codec_name);
        av_freep(&s);
    }
}

static void mediacodec_buffer_release(void *opaque, uint8_t *data)
{
    AVMediaCodecBuffer *buffer = opaque;
    MediaCodecDecContext *ctx = buffer->ctx;
    int released = atomic_load(&buffer->released);

    if (!released && (ctx->delay_flush || buffer->serial == atomic_load(&ctx->serial))) {
        atomic_fetch_sub(&ctx->hw_buffer_count, 1);
        av_log(ctx->avctx, AV_LOG_DEBUG,
               "Releasing output buffer %zd (%p) ts=%"PRId64" on free() [%d pending]\n",
               buffer->index, buffer, buffer->pts, atomic_load(&ctx->hw_buffer_count));
        ff_AMediaCodec_releaseOutputBuffer(ctx->codec, buffer->index, 0);
    }

    ff_mediacodec_dec_unref(ctx);
    av_freep(&buffer);
}

static int mediacodec_wrap_hw_buffer(AVCodecContext *avctx,
                                  MediaCodecDecContext *s,
                                  ssize_t index,
                                  FFAMediaCodecBufferInfo *info,
                                  AVFrame *frame)
{
    int ret = 0;
    int status = 0;
    AVMediaCodecBuffer *buffer = NULL;

    frame->buf[0] = NULL;
    frame->width = avctx->width;
    frame->height = avctx->height;
    frame->format = avctx->pix_fmt;
    frame->sample_aspect_ratio = avctx->sample_aspect_ratio;

    if (avctx->pkt_timebase.num && avctx->pkt_timebase.den) {
        frame->pts = av_rescale_q(info->presentationTimeUs,
                                      AV_TIME_BASE_Q,
                                      avctx->pkt_timebase);
    } else {
        frame->pts = info->presentationTimeUs;
    }
    frame->pkt_dts = AV_NOPTS_VALUE;
    frame->color_range = avctx->color_range;
    frame->color_primaries = avctx->color_primaries;
    frame->color_trc = avctx->color_trc;
    frame->colorspace = avctx->colorspace;

    buffer = av_mallocz(sizeof(AVMediaCodecBuffer));
    if (!buffer) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    atomic_init(&buffer->released, 0);

    frame->buf[0] = av_buffer_create(NULL,
                                     0,
                                     mediacodec_buffer_release,
                                     buffer,
                                     AV_BUFFER_FLAG_READONLY);

    if (!frame->buf[0]) {
        ret = AVERROR(ENOMEM);
        goto fail;

    }

    buffer->ctx = s;
    buffer->serial = atomic_load(&s->serial);
    ff_mediacodec_dec_ref(s);

    buffer->index = index;
    buffer->pts = info->presentationTimeUs;

    frame->data[3] = (uint8_t *)buffer;

    atomic_fetch_add(&s->hw_buffer_count, 1);
    av_log(avctx, AV_LOG_DEBUG,
            "Wrapping output buffer %zd (%p) ts=%"PRId64" [%d pending]\n",
            buffer->index, buffer, buffer->pts, atomic_load(&s->hw_buffer_count));

    return 0;
fail:
    av_freep(&buffer);
    status = ff_AMediaCodec_releaseOutputBuffer(s->codec, index, 0);
    if (status < 0) {
        av_log(avctx, AV_LOG_ERROR, "Failed to release output buffer\n");
        ret = AVERROR_EXTERNAL;
    }

    return ret;
}

static int mediacodec_wrap_sw_audio_buffer(AVCodecContext *avctx,
                                           MediaCodecDecContext *s,
                                           uint8_t *data,
                                           size_t size,
                                           ssize_t index,
                                           FFAMediaCodecBufferInfo *info,
                                           AVFrame *frame)
{
    int ret = 0;
    int status = 0;
    const int sample_size = av_get_bytes_per_sample(avctx->sample_fmt);
    if (!sample_size) {
        av_log(avctx, AV_LOG_ERROR, "Could not get bytes per sample\n");
        ret = AVERROR(ENOSYS);
        goto done;
    }

    if (info->size % (sample_size * avctx->ch_layout.nb_channels)) {
        av_log(avctx, AV_LOG_ERROR, "input is not a multiple of channels * sample_size\n");
        ret = AVERROR(EINVAL);
        goto done;
    }

    frame->format = avctx->sample_fmt;
    frame->sample_rate = avctx->sample_rate;
    frame->nb_samples = info->size / (sample_size * avctx->ch_layout.nb_channels);

    ret = av_channel_layout_copy(&frame->ch_layout, &avctx->ch_layout);
    if (ret < 0) {
        av_log(avctx, AV_LOG_ERROR, "Could not copy channel layout\n");
        goto done;
    }

    /* MediaCodec buffers needs to be copied to our own refcounted buffers
     * because the flush command invalidates all input and output buffers.
     */
    ret = ff_get_buffer(avctx, frame, 0);
    if (ret < 0) {
        av_log(avctx, AV_LOG_ERROR, "Could not allocate buffer\n");
        goto done;
    }

    /* Override frame->pts as ff_get_buffer will override its value based
     * on the last avpacket received which is not in sync with the frame:
     *   * N avpackets can be pushed before 1 frame is actually returned
     *   * 0-sized avpackets are pushed to flush remaining frames at EOS */
    if (avctx->pkt_timebase.num && avctx->pkt_timebase.den) {
        frame->pts = av_rescale_q(info->presentationTimeUs,
                                      AV_TIME_BASE_Q,
                                      avctx->pkt_timebase);
    } else {
        frame->pts = info->presentationTimeUs;
    }
    frame->pkt_dts = AV_NOPTS_VALUE;
    frame->flags |= AV_FRAME_FLAG_KEY;

    av_log(avctx, AV_LOG_TRACE,
           "Frame: format=%d channels=%d sample_rate=%d nb_samples=%d",
           avctx->sample_fmt, avctx->ch_layout.nb_channels, avctx->sample_rate, frame->nb_samples);

    memcpy(frame->data[0], data, info->size);

    ret = 0;
done:
    status = ff_AMediaCodec_releaseOutputBuffer(s->codec, index, 0);
    if (status < 0) {
        av_log(avctx, AV_LOG_ERROR, "Failed to release output buffer\n");
        ret = AVERROR_EXTERNAL;
    }

    return ret;
}

static int mediacodec_wrap_sw_video_buffer(AVCodecContext *avctx,
                                           MediaCodecDecContext *s,
                                           uint8_t *data,
                                           size_t size,
                                           ssize_t index,
                                           FFAMediaCodecBufferInfo *info,
                                           AVFrame *frame)
{
    int ret = 0;
    int status = 0;

    frame->width = avctx->width;
    frame->height = avctx->height;
    frame->format = avctx->pix_fmt;

    /* MediaCodec buffers needs to be copied to our own refcounted buffers
     * because the flush command invalidates all input and output buffers.
     */
    if ((ret = ff_get_buffer(avctx, frame, 0)) < 0) {
        av_log(avctx, AV_LOG_ERROR, "Could not allocate buffer\n");
        goto done;
    }

    /* Override frame->pkt_pts as ff_get_buffer will override its value based
     * on the last avpacket received which is not in sync with the frame:
     *   * N avpackets can be pushed before 1 frame is actually returned
     *   * 0-sized avpackets are pushed to flush remaining frames at EOS */
    if (avctx->pkt_timebase.num && avctx->pkt_timebase.den) {
        frame->pts = av_rescale_q(info->presentationTimeUs,
                                      AV_TIME_BASE_Q,
                                      avctx->pkt_timebase);
    } else {
        frame->pts = info->presentationTimeUs;
    }
    frame->pkt_dts = AV_NOPTS_VALUE;

    av_log(avctx, AV_LOG_TRACE,
            "Frame: width=%d stride=%d height=%d slice-height=%d "
            "crop-top=%d crop-bottom=%d crop-left=%d crop-right=%d encoder=%s "
            "destination linesizes=%d,%d,%d\n" ,
            avctx->width, s->stride, avctx->height, s->slice_height,
            s->crop_top, s->crop_bottom, s->crop_left, s->crop_right, s->codec_name,
            frame->linesize[0], frame->linesize[1], frame->linesize[2]);

    switch (s->color_format) {
    case COLOR_FormatYUV420Planar:
        ff_mediacodec_sw_buffer_copy_yuv420_planar(avctx, s, data, size, info, frame);
        break;
    case COLOR_FormatYUV420SemiPlanar:
    case COLOR_QCOM_FormatYUV420SemiPlanar:
    case COLOR_QCOM_FormatYUV420SemiPlanar32m:
        ff_mediacodec_sw_buffer_copy_yuv420_semi_planar(avctx, s, data, size, info, frame);
        break;
    case COLOR_TI_FormatYUV420PackedSemiPlanar:
    case COLOR_TI_FormatYUV420PackedSemiPlanarInterlaced:
        ff_mediacodec_sw_buffer_copy_yuv420_packed_semi_planar(avctx, s, data, size, info, frame);
        break;
    case COLOR_QCOM_FormatYUV420PackedSemiPlanar64x32Tile2m8ka:
        ff_mediacodec_sw_buffer_copy_yuv420_packed_semi_planar_64x32Tile2m8ka(avctx, s, data, size, info, frame);
        break;
    default:
        av_log(avctx, AV_LOG_ERROR, "Unsupported color format 0x%x (value=%d)\n",
            s->color_format, s->color_format);
        ret = AVERROR(EINVAL);
        goto done;
    }

    ret = 0;
done:
    status = ff_AMediaCodec_releaseOutputBuffer(s->codec, index, 0);
    if (status < 0) {
        av_log(avctx, AV_LOG_ERROR, "Failed to release output buffer\n");
        ret = AVERROR_EXTERNAL;
    }

    return ret;
}

static int mediacodec_wrap_sw_buffer(AVCodecContext *avctx,
                                     MediaCodecDecContext *s,
                                     uint8_t *data,
                                     size_t size,
                                     ssize_t index,
                                     FFAMediaCodecBufferInfo *info,
                                     AVFrame *frame)
{
    if (avctx->codec_type == AVMEDIA_TYPE_AUDIO)
        return mediacodec_wrap_sw_audio_buffer(avctx, s, data, size, index, info, frame);
    else if (avctx->codec_type == AVMEDIA_TYPE_VIDEO)
        return mediacodec_wrap_sw_video_buffer(avctx, s, data, size, index, info, frame);
    else
        av_assert0(0);
}

#define AMEDIAFORMAT_GET_INT32(name, key, mandatory) do {                              \
    int32_t value = 0;                                                                 \
    if (ff_AMediaFormat_getInt32(s->format, key, &value)) {                            \
        (name) = value;                                                                \
    } else if (mandatory) {                                                            \
        av_log(avctx, AV_LOG_ERROR, "Could not get %s from format %s\n", key, format); \
        ret = AVERROR_EXTERNAL;                                                        \
        goto fail;                                                                     \
    }                                                                                  \
} while (0)                                                                            \

static int mediacodec_dec_parse_video_format(AVCodecContext *avctx, MediaCodecDecContext *s)
{
    int ret = 0;
    int width = 0;
    int height = 0;
    int color_range = 0;
    int color_standard = 0;
    int color_transfer = 0;
    char *format = NULL;

    if (!s->format) {
        av_log(avctx, AV_LOG_ERROR, "Output MediaFormat is not set\n");
        return AVERROR(EINVAL);
    }

    format = ff_AMediaFormat_toString(s->format);
    if (!format) {
        return AVERROR_EXTERNAL;
    }
    av_log(avctx, AV_LOG_DEBUG, "Parsing MediaFormat %s\n", format);

    /* Mandatory fields */
    AMEDIAFORMAT_GET_INT32(s->width,  "width", 1);
    AMEDIAFORMAT_GET_INT32(s->height, "height", 1);

    AMEDIAFORMAT_GET_INT32(s->stride, "stride", 0);
    s->stride = s->stride > 0 ? s->stride : s->width;

    AMEDIAFORMAT_GET_INT32(s->slice_height, "slice-height", 0);

    if (strstr(s->codec_name, "OMX.Nvidia.") && s->slice_height == 0) {
        s->slice_height = FFALIGN(s->height, 16);
    } else if (strstr(s->codec_name, "OMX.SEC.avc.dec")) {
        s->slice_height = avctx->height;
        s->stride = avctx->width;
    } else if (s->slice_height == 0) {
        s->slice_height = s->height;
    }

    AMEDIAFORMAT_GET_INT32(s->color_format, "color-format", 1);
    avctx->pix_fmt = mcdec_map_color_format(avctx, s, s->color_format);
    if (avctx->pix_fmt == AV_PIX_FMT_NONE) {
        av_log(avctx, AV_LOG_ERROR, "Output color format is not supported\n");
        ret = AVERROR(EINVAL);
        goto fail;
    }

    /* Optional fields */
    AMEDIAFORMAT_GET_INT32(s->crop_top,    "crop-top",    0);
    AMEDIAFORMAT_GET_INT32(s->crop_bottom, "crop-bottom", 0);
    AMEDIAFORMAT_GET_INT32(s->crop_left,   "crop-left",   0);
    AMEDIAFORMAT_GET_INT32(s->crop_right,  "crop-right",  0);

    // Try "crop" for NDK
    // MediaTek SOC return some default value like Rect(0, 0, 318, 238)
    if (!(s->crop_right && s->crop_bottom) && s->use_ndk_codec && !strstr(s->codec_name, ".mtk."))
        ff_AMediaFormat_getRect(s->format, "crop", &s->crop_left, &s->crop_top, &s->crop_right, &s->crop_bottom);

    if (s->crop_right && s->crop_bottom) {
        width = s->crop_right + 1 - s->crop_left;
        height = s->crop_bottom + 1 - s->crop_top;
    } else {
        /* TODO: NDK MediaFormat should try getRect() first.
         * Try crop-width/crop-height, it works on NVIDIA Shield.
         */
        AMEDIAFORMAT_GET_INT32(width,  "crop-width",  0);
        AMEDIAFORMAT_GET_INT32(height, "crop-height", 0);
    }
    if (!width || !height) {
        width = s->width;
        height = s->height;
    }

    AMEDIAFORMAT_GET_INT32(s->display_width,  "display-width",  0);
    AMEDIAFORMAT_GET_INT32(s->display_height, "display-height", 0);

    if (s->display_width && s->display_height) {
        AVRational sar = av_div_q(
            (AVRational){ s->display_width, s->display_height },
            (AVRational){ width, height });
        ff_set_sar(avctx, sar);
    }

    AMEDIAFORMAT_GET_INT32(color_range, "color-range", 0);
    if (color_range)
        avctx->color_range = ff_AMediaFormatColorRange_to_AVColorRange(color_range);

    AMEDIAFORMAT_GET_INT32(color_standard, "color-standard", 0);
    if (color_standard) {
        avctx->colorspace = ff_AMediaFormatColorStandard_to_AVColorSpace(color_standard);
        avctx->color_primaries = ff_AMediaFormatColorStandard_to_AVColorPrimaries(color_standard);
    }

    AMEDIAFORMAT_GET_INT32(color_transfer, "color-transfer", 0);
    if (color_transfer)
        avctx->color_trc = ff_AMediaFormatColorTransfer_to_AVColorTransfer(color_transfer);

    av_log(avctx, AV_LOG_INFO,
        "Output crop parameters top=%d bottom=%d left=%d right=%d, "
        "resulting dimensions width=%d height=%d\n",
        s->crop_top, s->crop_bottom, s->crop_left, s->crop_right,
        width, height);

    av_freep(&format);
    return ff_set_dimensions(avctx, width, height);
fail:
    av_freep(&format);
    return ret;
}

static int mediacodec_dec_parse_audio_format(AVCodecContext *avctx, MediaCodecDecContext *s)
{
    int ret = 0;
    int sample_rate = 0;
    int channel_count = 0;
    int channel_mask = 0;
    int pcm_encoding = 0;
    char *format = NULL;

    if (!s->format) {
        av_log(avctx, AV_LOG_ERROR, "Output MediaFormat is not set\n");
        return AVERROR(EINVAL);
    }

    format = ff_AMediaFormat_toString(s->format);
    if (!format) {
        return AVERROR_EXTERNAL;
    }
    av_log(avctx, AV_LOG_DEBUG, "Parsing MediaFormat %s\n", format);

    /* Mandatory fields */
    AMEDIAFORMAT_GET_INT32(channel_count, "channel-count", 1);
    AMEDIAFORMAT_GET_INT32(sample_rate,   "sample-rate",   1);

    AMEDIAFORMAT_GET_INT32(pcm_encoding, "pcm-encoding", 0);
    if (pcm_encoding)
        avctx->sample_fmt  = mcdec_map_pcm_format(avctx, s, pcm_encoding);
    else
        avctx->sample_fmt = AV_SAMPLE_FMT_S16;

    avctx->sample_rate = sample_rate;

    AMEDIAFORMAT_GET_INT32(channel_mask, "channel-mask", 0);
    if (channel_mask)
        av_channel_layout_from_mask(&avctx->ch_layout, mcdec_map_channel_mask(avctx, channel_mask));
    else
        av_channel_layout_default(&avctx->ch_layout, channel_count);

    av_log(avctx, AV_LOG_INFO,
        "Output parameters channel-count=%d channel-layout=%x sample-rate=%d\n",
        channel_count, channel_mask, sample_rate);

fail:
    av_freep(&format);
    return ret;
}

static int mediacodec_dec_parse_format(AVCodecContext *avctx, MediaCodecDecContext *s)
{
    if (avctx->codec_type == AVMEDIA_TYPE_AUDIO)
        return mediacodec_dec_parse_audio_format(avctx, s);
    else if (avctx->codec_type == AVMEDIA_TYPE_VIDEO)
        return mediacodec_dec_parse_video_format(avctx, s);
    else
        av_assert0(0);
}

static int mediacodec_dec_flush_codec(AVCodecContext *avctx, MediaCodecDecContext *s)
{
    FFAMediaCodec *codec = s->codec;
    int status;

    s->output_buffer_count = 0;

    s->draining = 0;
    s->flushing = 0;
    s->eos = 0;
    atomic_fetch_add(&s->serial, 1);
    atomic_init(&s->hw_buffer_count, 0);
    s->current_input_buffer = -1;

    status = ff_AMediaCodec_flush(codec);
    native_dv_diag_flush(avctx, &s->native_dv_diag, status);
    if (status < 0) {
        av_log(avctx, AV_LOG_ERROR, "Failed to flush codec\n");
        return AVERROR_EXTERNAL;
    }

    return 0;
}

static int mediacodec_dec_get_video_codec(AVCodecContext *avctx, MediaCodecDecContext *s,
                                          const char *mime, FFAMediaFormat *format)
{
    int profile;
    int native_dv = !strcmp(mime, "video/dolby-vision");

    enum AVPixelFormat pix_fmt;
    static const enum AVPixelFormat pix_fmts[] = {
        AV_PIX_FMT_MEDIACODEC,
        AV_PIX_FMT_NONE,
    };

    pix_fmt = ff_get_format(avctx, pix_fmts);
    if (pix_fmt == AV_PIX_FMT_MEDIACODEC) {
        AVMediaCodecContext *user_ctx = avctx->hwaccel_context;

        if (avctx->hw_device_ctx) {
            AVHWDeviceContext *device_ctx = (AVHWDeviceContext*)(avctx->hw_device_ctx->data);
            if (device_ctx->type == AV_HWDEVICE_TYPE_MEDIACODEC) {
                if (device_ctx->hwctx) {
                    AVMediaCodecDeviceContext *mediacodec_ctx = (AVMediaCodecDeviceContext *)device_ctx->hwctx;
                    s->surface = ff_mediacodec_surface_ref(mediacodec_ctx->surface, mediacodec_ctx->native_window, avctx);
                    av_log(avctx, AV_LOG_INFO, "Using surface %p\n", s->surface);
                }
            }
        }

        if (!s->surface && user_ctx && user_ctx->surface) {
            s->surface = ff_mediacodec_surface_ref(user_ctx->surface, NULL, avctx);
            av_log(avctx, AV_LOG_INFO, "Using surface %p\n", s->surface);
        }
    }

    if (native_dv) {
        int32_t format_profile = 0;

        if (!s->surface) {
            av_log(avctx, AV_LOG_ERROR, "native_dv requires Surface output; copy decoding is unsupported\n");
            return AVERROR(EINVAL);
        }
        if (avctx->codec_id != AV_CODEC_ID_HEVC) {
            av_log(avctx, AV_LOG_ERROR, "native_dv requires an HEVC decoder\n");
            return AVERROR(EINVAL);
        }
        if (ff_AMediaFormat_getInt32(format, "profile", &format_profile) &&
            format_profile != 0x20 && format_profile != 0x2 &&
            format_profile != 0x100) {
            av_log(avctx, AV_LOG_ERROR, "native_dv received an unexpected Android profile 0x%x\n",
                   format_profile);
            return AVERROR_INVALIDDATA;
        }
        if (format_profile == 0x20) {
            /* P5: explicit DolbyVisionProfileDvheStn, matching the declared
             * decoder capability. */
            profile = format_profile;
            av_log(avctx, AV_LOG_INFO,
                   "native_dv selecting Surface decoder: MIME=%s, profile=0x%x\n", mime, profile);
        } else {
            /* P8: configure with the modern-SDK DvheSt key (0x100). On the
             * API 24 test device this is refused at configure (fail-fast);
             * accepted signaling (0x20) stalls presentation instead. The
             * lookup stays MIME-only (profile < 0) because declared
             * capabilities omit P8 keys; 0x2 is retained in the whitelist
             * only as the API 24-era experimental value. */
            profile = -1;
            av_log(avctx, AV_LOG_INFO,
                   "native_dv selecting Surface decoder: MIME=%s, configure profile=0x%x (MIME-only lookup)\n",
                   mime, format_profile);
        }
    } else {
        profile = ff_AMediaCodecProfile_getProfileFromAVCodecContext(avctx);
        if (profile < 0)
            av_log(avctx, AV_LOG_WARNING, "Unsupported or unknown profile\n");
    }

    s->codec_name = ff_AMediaCodecList_getCodecNameByType(mime, profile, 0, avctx);
    if (!s->codec_name) {
        if (native_dv) {
            av_log(avctx, AV_LOG_ERROR,
                   "native_dv found no matching decoder for MIME=%s profile=0x%x; no fallback\n",
                   mime, profile);
            return AVERROR_EXTERNAL;
        }
        // getCodecNameByType() can fail due to missing JVM, while NDK
        // mediacodec can be used without JVM.
        if (!s->use_ndk_codec) {
            return AVERROR_EXTERNAL;
        }
        av_log(avctx, AV_LOG_INFO, "Failed to getCodecNameByType\n");
    } else {
        av_log(avctx, AV_LOG_DEBUG, "Found decoder %s\n", s->codec_name);
    }

    if (native_dv)
        av_log(avctx, AV_LOG_INFO, "native_dv selected codec=%s MIME=%s profile=0x%x\n",
               s->codec_name, mime, profile);

    if (s->codec_name)
        s->codec = ff_AMediaCodec_createCodecByName(s->codec_name, s->use_ndk_codec);
    else {
        s->codec = ff_AMediaCodec_createDecoderByType(mime, s->use_ndk_codec);
        if (s->codec) {
            s->codec_name = ff_AMediaCodec_getName(s->codec);
            if (!s->codec_name)
                s->codec_name = av_strdup(mime);
        }
    }
    if (!s->codec) {
        av_log(avctx, AV_LOG_ERROR, "Failed to create media decoder for type %s and name %s\n", mime, s->codec_name);
        return AVERROR_EXTERNAL;
    }

    return 0;
}

static int mediacodec_dec_get_audio_codec(AVCodecContext *avctx, MediaCodecDecContext *s,
                                          const char *mime, FFAMediaFormat *format)
{
    s->codec = ff_AMediaCodec_createDecoderByType(mime, s->use_ndk_codec);
    if (!s->codec) {
        av_log(avctx, AV_LOG_ERROR, "Failed to create media decoder for mime %s\n", mime);
        return AVERROR_EXTERNAL;
    }

    s->codec_name = ff_AMediaCodec_getName(s->codec);
    if (!s->codec_name) {
        s->codec_name = av_strdup(mime);
        if (!s->codec_name)
            return AVERROR(ENOMEM);
    }

    return 0;
}

/*
 * HEVC SPS VUI color-descriptor probe.
 *
 * MediaCodec does not parse the bitstream VUI, and when the container color
 * metadata never reaches avctx (avctx trc/primaries UNSPECIFIED) hardware
 * frames come back as SDR/bt.1886 for pure HDR HEVC streams. As a fallback,
 * parse the SPS -> VUI colour description directly from extradata so the
 * color keys further down in ff_mediacodec_dec_init can be populated.
 *
 * The block below (MKSVUIGetBits .. mcdec_probe_hevc_vui_color) is
 * self-contained: it does not use any Android or FFmpeg API beyond the
 * avctx fields, so it can be extracted verbatim for host-side testing.
 */

typedef struct MKSVUIGetBits {
    const uint8_t *buf;
    int size_bits;
    int index;
    int error;
} MKSVUIGetBits;

/* Bit reader with a sticky overread flag: once error is set, every read
 * returns 0 and every parse ends in failure. Never reads out of bounds. */
static int mkvui_get_bits(MKSVUIGetBits *gb, int n)
{
    uint32_t value = 0;

    if (gb->error || n < 1 || n > 32 || n > gb->size_bits - gb->index) {
        gb->error = 1;
        return 0;
    }
    while (n > 0) {
        int take = FFMIN(8 - (gb->index & 7), n);
        int shift = 8 - (gb->index & 7) - take;

        value = (value << take) |
                ((gb->buf[gb->index >> 3] >> shift) & ((1 << take) - 1));
        gb->index += take;
        n -= take;
    }
    return (int)value;
}

static void mkvui_skip_bits(MKSVUIGetBits *gb, int n)
{
    while (n > 0 && !gb->error) {
        int take = FFMIN(n, 32);

        mkvui_get_bits(gb, take);
        n -= take;
    }
}

static int mkvui_get_ue(MKSVUIGetBits *gb)
{
    int zeros = 0;

    while (!gb->error && mkvui_get_bits(gb, 1) == 0) {
        if (++zeros > 30) {
            gb->error = 1;
            return 0;
        }
    }
    if (gb->error)
        return 0;
    if (zeros == 0)
        return 0;
    return (1 << zeros) - 1 + mkvui_get_bits(gb, zeros);
}

static int mkvui_get_se(MKSVUIGetBits *gb)
{
    int k = mkvui_get_ue(gb);

    if (gb->error)
        return 0;
    /* 0 -> 0, odd -> (k+1)/2, even -> -k/2 */
    return (k & 1) ? (k + 1) / 2 : -(k >> 1);
}

/* Remove emulation prevention bytes: 00 00 03 -> 00 00, mirroring
 * ff_h2645_extract_rbsp(). Returns the RBSP length, < 0 on overflow. */
static int mkvui_unescape(const uint8_t *src, int src_len,
                          uint8_t *dst, int dst_size)
{
    int si = 0, di = 0;

    while (si < src_len) {
        if (si + 2 < src_len &&
            src[si] == 0 && src[si + 1] == 0 && src[si + 2] == 3) {
            if (di + 2 > dst_size)
                return -1;
            dst[di++] = 0;
            dst[di++] = 0;
            si += 3;
            continue;
        }
        if (di + 1 > dst_size)
            return -1;
        dst[di++] = src[si++];
    }
    return di;
}

/* Consume one sub-layer's SchedSelEntry list (Annex E.2): cpb_cnt+1 times
 * bit_rate/cpb_size (plus the du pair when sub-pic HRD is signalled) and a
 * 1-bit cbr_flag each. Called once per present HRD type: the NAL and the
 * VCL lists are separate syntax repetitions, not a single shared one. */
static void mkvui_cpb_entries(MKSVUIGetBits *gb, int sub_pic, int cpb_cnt)
{
    for (int c = 0; c <= cpb_cnt; c++) {
        mkvui_get_ue(gb);    /* bit_rate_value_minus1 */
        mkvui_get_ue(gb);    /* cpb_size_value_minus1 */
        if (sub_pic) {
            mkvui_get_ue(gb);  /* cpb_size_du_value_minus1 */
            mkvui_get_ue(gb);  /* bit_rate_du_value_minus1 */
        }
        mkvui_get_bits(gb, 1);  /* cbr_flag */
    }
}

/* Parse one SPS NAL (including the 2-byte NAL unit header) and extract the
 * VUI colour description. Field order mirrors hevc/ps.c ff_hevc_parse_sps
 * (and its short-term RPS / profile_tier_level helpers) bit for bit; values
 * are consumed without being stored. Returns 0 and fills trc/prim/matrix
 * when the VUI colour description was found and the bitstream stayed in
 * bounds; < 0 on any parse failure. */
static int mkvui_parse_sps_color(const uint8_t *nal, int nal_len,
                                 int *trc, int *prim, int *matrix)
{
    uint8_t rbsp[1024];
    MKSVUIGetBits gb;
    int rbsp_len, i, j;
    int max_sub_layers, log2_max_poc_lsb, nb_st_rps, prev_num_delta_pocs;

    if (nal_len < 4) /* 2-byte NAL header + minimal payload */
        return -1;
    rbsp_len = mkvui_unescape(nal + 2, nal_len - 2, rbsp, sizeof(rbsp));
    if (rbsp_len < 0)
        return -1;

    memset(&gb, 0, sizeof(gb));
    gb.buf      = rbsp;
    gb.size_bits = rbsp_len * 8;

    mkvui_get_bits(&gb, 4);  /* sps_video_parameter_set_id */
    max_sub_layers = mkvui_get_bits(&gb, 3) + 1;
    mkvui_get_bits(&gb, 1);  /* temporal_id_nesting */
    if (gb.error || max_sub_layers > HEVC_MAX_SUB_LAYERS)
        return -1;

    /* profile_tier_level(1, max_sub_layers): general part is a fixed
     * 2+1+5 + 32 + 48 + 8 bits (the constraint flags block totals exactly
     * 48 bits in every branch of decode_profile_tier_level). */
    mkvui_skip_bits(&gb, 2 + 1 + 5 + 32 + 48);  /* profile_space..inbld */
    mkvui_get_bits(&gb, 8);  /* general_level_idc */
    if (max_sub_layers > 1) {
        int sub_profile_present[HEVC_MAX_SUB_LAYERS] = { 0 };
        int sub_level_present[HEVC_MAX_SUB_LAYERS]   = { 0 };

        for (i = 0; i < max_sub_layers - 1; i++) {
            sub_profile_present[i] = mkvui_get_bits(&gb, 1);
            sub_level_present[i]   = mkvui_get_bits(&gb, 1);
        }
        for (i = max_sub_layers - 1; i < 8; i++)
            mkvui_get_bits(&gb, 2);  /* reserved_zero_2bits */
        for (i = 0; i < max_sub_layers - 1; i++) {
            if (sub_profile_present[i])
                mkvui_skip_bits(&gb, 88);  /* sub_layer profile_tier_level */
            if (sub_level_present[i])
                mkvui_get_bits(&gb, 8);    /* sub_layer_level_idc */
        }
    }
    if (gb.error)
        return -1;

    mkvui_get_ue(&gb);  /* sps_seq_parameter_set_id */
    if (mkvui_get_ue(&gb) == 3)  /* chroma_format_idc */
        mkvui_get_bits(&gb, 1);  /* separate_colour_plane_flag */
    mkvui_get_ue(&gb);  /* pic_width_in_luma_samples */
    mkvui_get_ue(&gb);  /* pic_height_in_luma_samples */
    if (mkvui_get_bits(&gb, 1)) {  /* conformance_window_flag */
        for (i = 0; i < 4; i++)
            mkvui_get_ue(&gb);
    }
    mkvui_get_ue(&gb);  /* bit_depth_luma_minus8 */
    mkvui_get_ue(&gb);  /* bit_depth_chroma_minus8 */
    log2_max_poc_lsb = mkvui_get_ue(&gb) + 4;
    if (gb.error || log2_max_poc_lsb > 16)
        return -1;

    if (mkvui_get_bits(&gb, 1)) {  /* sps_sub_layer_ordering_info_present */
        for (i = 0; i < max_sub_layers; i++) {
            mkvui_get_ue(&gb);
            mkvui_get_ue(&gb);
            mkvui_get_ue(&gb);
        }
    } else {
        for (i = max_sub_layers - 1; i < max_sub_layers; i++) {
            mkvui_get_ue(&gb);
            mkvui_get_ue(&gb);
            mkvui_get_ue(&gb);
        }
    }
    for (i = 0; i < 6; i++)  /* log2_min/max cb/tb sizes, transform depths */
        mkvui_get_ue(&gb);

    if (mkvui_get_bits(&gb, 1)) {  /* scaling_list_enabled */
        if (mkvui_get_bits(&gb, 1)) {  /* scaling_list_data_present */
            /* scaling_list_data(), mirror of hevc/ps.c: consume only. */
            for (i = 0; i < 4; i++) {  /* size_id */
                for (j = 0; j < 6; j += (i == 3) ? 3 : 1) {  /* matrix_id */
                    if (mkvui_get_bits(&gb, 1)) {  /* pred_mode */
                        int coef_num = (i == 0) ? 16 : 64;

                        if (i > 1)
                            mkvui_get_se(&gb);  /* dc coefficient */
                        while (coef_num--)
                            mkvui_get_se(&gb);
                    } else {
                        mkvui_get_ue(&gb);  /* delta from previous matrix */
                    }
                }
            }
        }
    }

    mkvui_get_bits(&gb, 1);  /* amp_enabled */
    mkvui_get_bits(&gb, 1);  /* sample_adaptive_offset_enabled */

    if (mkvui_get_bits(&gb, 1)) {  /* pcm_enabled */
        mkvui_get_bits(&gb, 4);  /* pcm_sample_bit_depth_luma_minus1 */
        mkvui_get_bits(&gb, 4);  /* pcm_sample_bit_depth_chroma_minus1 */
        mkvui_get_ue(&gb);       /* log2_min_pcm_luma_coding_block_size_minus3 */
        mkvui_get_ue(&gb);       /* log2_diff_max_min_pcm_luma_... */
        mkvui_get_bits(&gb, 1);  /* pcm_loop_filter_disabled */
    }

    nb_st_rps = mkvui_get_ue(&gb);
    if (gb.error || nb_st_rps > HEVC_MAX_SHORT_TERM_REF_PIC_SETS)
        return -1;

    /* short_term_ref_pic_sets, mirror of ff_hevc_decode_short_term_rps()
     * in the SPS context (is_slice_header == 0): only the previous RPS
     * can be a predictor and only its num_delta_pocs is needed to walk
     * the syntax. */
    prev_num_delta_pocs = 0;
    for (i = 0; i < nb_st_rps; i++) {
        int num_delta_pocs = 0;

        if (i > 0 && mkvui_get_bits(&gb, 1)) { /* inter_ref_pic_set_prediction */
            int abs_delta_rps;
            int k = 0;
            uint8_t used[32] = { 0 };

            mkvui_get_bits(&gb, 1); /* delta_rps_sign, consumed only */
            abs_delta_rps = mkvui_get_ue(&gb) + 1;
            if (gb.error || abs_delta_rps > 32768)
                return -1;
            for (j = 0; j <= prev_num_delta_pocs; j++) {
                int use_delta;

                used[k] = mkvui_get_bits(&gb, 1);
                use_delta = used[k] ? 0 : mkvui_get_bits(&gb, 1);
                if (used[k] || use_delta)
                    k++;
            }
            if (gb.error || k >= (int)FF_ARRAY_ELEMS(used))
                return -1;
            num_delta_pocs = k;
        } else {
            int num_negative_pics = mkvui_get_ue(&gb);
            int num_positive_pics = mkvui_get_ue(&gb);

            if (gb.error || num_negative_pics >= HEVC_MAX_REFS ||
                num_positive_pics >= HEVC_MAX_REFS)
                return -1;
            /* 7.3.7: each delta_poc is followed by a 1-bit
             * used_by_curr_pic_s0/s1_flag; skipping them would desync
             * every later field (mirrors ps.c st_rps used-flag reads). */
            for (j = 0; j < num_negative_pics; j++) {
                if (mkvui_get_ue(&gb) >= 32768)  /* delta_poc_s0_minus1 */
                    return -1;
                mkvui_get_bits(&gb, 1);  /* used_by_curr_pic_s0_flag */
            }
            for (j = 0; j < num_positive_pics; j++) {
                if (mkvui_get_ue(&gb) >= 32768)  /* delta_poc_s1_minus1 */
                    return -1;
                mkvui_get_bits(&gb, 1);  /* used_by_curr_pic_s1_flag */
            }
            if (gb.error)
                return -1;
            num_delta_pocs = num_negative_pics + num_positive_pics;
        }
        prev_num_delta_pocs = num_delta_pocs;
    }

    if (mkvui_get_bits(&gb, 1)) {  /* long_term_ref_pics_present */
        int num_long_term = mkvui_get_ue(&gb);

        if (gb.error || num_long_term > HEVC_MAX_LONG_TERM_REF_PICS)
            return -1;
        for (i = 0; i < num_long_term; i++) {
            mkvui_get_bits(&gb, log2_max_poc_lsb);  /* lt_ref_pic_poc_lsb_sps */
            mkvui_get_bits(&gb, 1);  /* used_by_curr_pic_lt_flag */
        }
    }

    mkvui_get_bits(&gb, 1);  /* sps_temporal_mvp_enabled */
    mkvui_get_bits(&gb, 1);  /* strong_intra_smoothing_enabled */

    if (!mkvui_get_bits(&gb, 1))  /* vui_parameters_present */
        return -1;

    /* vui_parameters(), field order mirrors ff_h2645_decode_common_vui_params
     * and decode_vui(). The alternate-syntax workarounds of decode_vui are
     * intentionally not reproduced: this probe only needs well-formed VUI. */
    if (mkvui_get_bits(&gb, 1)) {  /* aspect_ratio_info_present */
        if (mkvui_get_bits(&gb, 8) == 255) {  /* aspect_ratio_idc == EXTENDED_SAR */
            mkvui_get_bits(&gb, 16);
            mkvui_get_bits(&gb, 16);
        }
    }
    if (mkvui_get_bits(&gb, 1))  /* overscan_info_present */
        mkvui_get_bits(&gb, 1);  /* overscan_appropriate */

    if (mkvui_get_bits(&gb, 1)) {  /* video_signal_type_present */
        mkvui_get_bits(&gb, 3);  /* video_format */
        mkvui_get_bits(&gb, 1);  /* video_full_range */
        if (mkvui_get_bits(&gb, 1)) {  /* colour_description_present */
            *prim   = mkvui_get_bits(&gb, 8);  /* colour_primaries */
            *trc    = mkvui_get_bits(&gb, 8);  /* transfer_characteristics */
            *matrix = mkvui_get_bits(&gb, 8);  /* matrix_coeffs */
        }
    }

    if (mkvui_get_bits(&gb, 1)) {  /* chroma_loc_info_present */
        mkvui_get_ue(&gb);
        mkvui_get_ue(&gb);
    }
    mkvui_get_bits(&gb, 1);  /* neutral_chroma_indication */
    mkvui_get_bits(&gb, 1);  /* field_seq */
    mkvui_get_bits(&gb, 1);  /* frame_field_info_present */
    if (mkvui_get_bits(&gb, 1)) {  /* default_display_window */
        for (i = 0; i < 4; i++)
            mkvui_get_ue(&gb);
    }

    if (mkvui_get_bits(&gb, 1)) {  /* vui_timing_info_present */
        mkvui_get_bits(&gb, 32);  /* num_units_in_tick */
        mkvui_get_bits(&gb, 32);  /* time_scale */
        if (mkvui_get_bits(&gb, 1))  /* poc_proportional_to_timing */
            mkvui_get_ue(&gb);
        if (mkvui_get_bits(&gb, 1)) {  /* vui_hrd_parameters_present */
            /* HRD syntax consumed without storage, mirroring
             * hevc/ps.c decode_hrd(gb, 1, ...) + decode_sublayer_hrd(). */
            int nal_hrd, vcl_hrd, sub_pic = 0;
            int cpb_cnts[HEVC_MAX_SUB_LAYERS] = { 0 };
            int sl;

            nal_hrd = mkvui_get_bits(&gb, 1);
            vcl_hrd = mkvui_get_bits(&gb, 1);
            if (nal_hrd || vcl_hrd) {
                sub_pic = mkvui_get_bits(&gb, 1);
                if (sub_pic) {
                    mkvui_get_bits(&gb, 8);   /* tick_divisor_minus2 */
                    mkvui_get_bits(&gb, 5);   /* du_cpb_removal_delay... */
                    mkvui_get_bits(&gb, 1);   /* sub_pic_cpb_params... */
                    mkvui_get_bits(&gb, 5);   /* dpb_output_delay_du... */
                }
                mkvui_get_bits(&gb, 4);       /* bit_rate_scale */
                mkvui_get_bits(&gb, 4);       /* cpb_size_scale */
                if (sub_pic)
                    mkvui_get_bits(&gb, 4);   /* cpb_size_du_scale */
                mkvui_get_bits(&gb, 5);       /* initial_cpb_removal... */
                mkvui_get_bits(&gb, 5);       /* au_cpb_removal_delay... */
                mkvui_get_bits(&gb, 5);       /* dpb_output_delay... */
            }
            for (sl = 0; sl < max_sub_layers; sl++) {
                int fixed = mkvui_get_bits(&gb, 1);
                int within = fixed ? 0 : mkvui_get_bits(&gb, 1);
                int low_delay = 0;

                if (within || fixed)
                    mkvui_get_ue(&gb);        /* elemental_duration_in_tc */
                else
                    low_delay = mkvui_get_bits(&gb, 1);
                if (!low_delay) {
                    cpb_cnts[sl] = mkvui_get_ue(&gb);
                    if (gb.error || cpb_cnts[sl] > 31)
                        return -1;
                }
                if (gb.error)
                    return -1;
                if (nal_hrd)
                    mkvui_cpb_entries(&gb, sub_pic, cpb_cnts[sl]);
                if (vcl_hrd)
                    mkvui_cpb_entries(&gb, sub_pic, cpb_cnts[sl]);
            }
        }
    }

    if (mkvui_get_bits(&gb, 1)) {  /* bitstream_restriction */
        mkvui_get_bits(&gb, 1);  /* tiles_fixed_structure */
        mkvui_get_bits(&gb, 1);  /* motion_vectors_over_pic_boundaries */
        mkvui_get_bits(&gb, 1);  /* restricted_ref_pic_lists */
        for (i = 0; i < 5; i++)
            mkvui_get_ue(&gb);
    }

    return gb.error ? -1 : 0;
}

/* Walk every NAL unit in extradata (hvcC or Annex-B layout) and return the
 * colour description of the first SPS whose VUI parses cleanly. */
static int mkvui_probe_hevc_extradata(const uint8_t *data, int size,
                                      int *trc, int *prim, int *matrix)
{
    if (size >= 24 && data[0] == 1) {
        /* hvcC (ISO 14496-15): 22-byte header, numOfArrays at offset 22,
         * each array: 1-byte flags+type, u16 numNalus, per NAL u16 length
         * + data. The NAL payloads here are raw (pre-Annex-B) units. */
        int num_arrays = data[22];
        int pos = 23;
        int a, n;

        for (a = 0; a < num_arrays && pos + 3 <= size; a++) {
            int nal_type   = data[pos] & 0x3f;
            int num_nalus  = (data[pos + 1] << 8) | data[pos + 2];

            pos += 3;
            for (n = 0; n < num_nalus && pos + 2 <= size; n++) {
                int nal_len = (data[pos] << 8) | data[pos + 1];

                pos += 2;
                if (pos + nal_len > size)
                    return -1;
                if (nal_type == 33 &&
                    mkvui_parse_sps_color(data + pos, nal_len,
                                          trc, prim, matrix) == 0)
                    return 0;
                pos += nal_len;
            }
        }
        return -1;
    }

    /* Annex-B: scan all start codes (00 00 01; a 00 00 00 01 prefix simply
     * leaves one extra zero byte that gets stripped below). */
    {
        int i = 0;

        while (i + 2 < size) {
            if (!data[i] && !data[i + 1] && data[i + 2] == 1) {
                int nal_start = i + 3;
                int nal_end   = size;
                int j;

                for (j = nal_start; j + 2 < size; j++) {
                    if (!data[j] && !data[j + 1] && data[j + 2] == 1) {
                        nal_end = j;
                        break;
                    }
                }
                while (nal_end > nal_start && data[nal_end - 1] == 0)
                    nal_end--;  /* zeros belong to the next start code */
                if (nal_end - nal_start >= 4 &&
                    ((data[nal_start] >> 1) & 0x3f) == 33 &&
                    mkvui_parse_sps_color(data + nal_start,
                                          nal_end - nal_start,
                                          trc, prim, matrix) == 0)
                    return 0;
                i = nal_end;
            } else {
                i++;
            }
        }
    }
    return -1;
}

/* Returns 0 when trc/prim/matrix were recovered from the extradata VUI and
 * all three are valid, non-unspecified values; < 0 otherwise. */
static int mcdec_probe_hevc_vui_color(AVCodecContext *avctx,
                                      int *trc, int *prim, int *matrix)
{
    int t = -1, p = -1, m = -1;

    if (!avctx->extradata || avctx->extradata_size <= 0)
        return AVERROR(EINVAL);
    if (mkvui_probe_hevc_extradata(avctx->extradata, avctx->extradata_size,
                                   &t, &p, &m) < 0)
        return AVERROR_INVALIDDATA;
    if (t == AVCOL_TRC_UNSPECIFIED || !av_color_transfer_name(t) ||
        p == AVCOL_PRI_UNSPECIFIED || !av_color_primaries_name(p) ||
        m == AVCOL_SPC_UNSPECIFIED || !av_color_space_name(m))
        return AVERROR_INVALIDDATA;
    *trc    = t;
    *prim   = p;
    *matrix = m;
    return 0;
}

int ff_mediacodec_dec_init(AVCodecContext *avctx, MediaCodecDecContext *s,
                           const char *mime, FFAMediaFormat *format)
{
    int ret;
    int status;

    s->avctx = avctx;
    atomic_init(&s->refcount, 1);
    atomic_init(&s->hw_buffer_count, 0);
    atomic_init(&s->serial, 1);
    s->current_input_buffer = -1;

    if (avctx->codec_type == AVMEDIA_TYPE_AUDIO)
        ret = mediacodec_dec_get_audio_codec(avctx, s, mime, format);
    else if (avctx->codec_type == AVMEDIA_TYPE_VIDEO)
        ret = mediacodec_dec_get_video_codec(avctx, s, mime, format);
    else
        av_assert0(0);
    if (ret < 0)
        goto fail;

    if (s->native_dv_diag.enabled)
        ff_mediacodec_diag_log(avctx, &s->native_dv_diag,
               "native_dv_diag decoder=%"PRIu64" epoch=%d event=configure mime=%s profile=32 surface_present=%d\n",
               s->native_dv_diag.decoder_id, s->native_dv_diag.epoch, mime, !!s->surface);

    /* MediaCodec does not parse the bitstream VUI itself: per the Android
     * docs the app must set the color keys on the configure MediaFormat,
     * otherwise the decoder will not echo color metadata back on output
     * frames (container smpte2084/bt2020 streams decode as bt.1886/SDR on
     * hwdec paths). Propagate the explicit values avctx already carries
     * (container colr box / codec API signaling) into the format.
     * Only set keys when avctx carries explicit values: UNSPECIFIED stays
     * unspecified, so there is zero behavior change for legacy streams.
     * Numeric values below are Android MediaFormat constants, mirroring the
     * FFAMediaFormatColor* enums in mediacodec_wrapper.h (sourced from AOSP
     * MediaFormat: COLOR_TRANSFER_SDR_VIDEO=3, COLOR_TRANSFER_ST2084=6,
     * COLOR_TRANSFER_HLG=7, COLOR_STANDARD_BT709=1, COLOR_STANDARD_BT2020=6,
     * COLOR_RANGE_FULL=1, COLOR_RANGE_LIMITED=2). */
    if (avctx->codec_type == AVMEDIA_TYPE_VIDEO) {
        int format_color_transfer = -1;
        int format_color_standard = -1;
        int format_color_range    = -1;

        /* MediaCodec also never reads the HEVC bitstream VUI itself: when
         * no container color metadata reached avctx (UNSPECIFIED above),
         * recover the SPS VUI colour description straight from extradata
         * so the keys below still get set for pure HDR HEVC streams.
         * Values missing from the bitstream or invalid stay UNSPECIFIED. */
        if (avctx->codec_id == AV_CODEC_ID_HEVC &&
            avctx->extradata && avctx->extradata_size > 0 &&
            avctx->color_trc == AVCOL_TRC_UNSPECIFIED &&
            avctx->color_primaries == AVCOL_PRI_UNSPECIFIED) {
            int vui_trc = -1, vui_prim = -1, vui_matrix = -1;

            if (mcdec_probe_hevc_vui_color(avctx, &vui_trc, &vui_prim,
                                           &vui_matrix) == 0) {
                avctx->color_trc        = vui_trc;
                avctx->color_primaries  = vui_prim;
                avctx->colorspace       = vui_matrix;
                av_log(avctx, AV_LOG_ERROR,
                       "MKSVUIPROBE: trc=%d prim=%d matrix=%d\n",
                       vui_trc, vui_prim, vui_matrix);
            } else {
                av_log(avctx, AV_LOG_DEBUG,
                       "MKSVUIPROBE: no usable SPS VUI color descriptor\n");
            }
        }

        av_log(avctx, AV_LOG_ERROR,
               "MKSCOLORKEYS: avctx trc=%d prim=%d range=%d\n",
               avctx->color_trc, avctx->color_primaries,
               avctx->color_range);

        switch (avctx->color_trc) {
        case AVCOL_TRC_SMPTE2084:    /* 16 */
            format_color_transfer = 6; /* COLOR_TRANSFER_ST2084 */
            break;
        case AVCOL_TRC_ARIB_STD_B67: /* 18, HLG */
            format_color_transfer = 7; /* COLOR_TRANSFER_HLG */
            break;
        /* Known SDR transfers map to COLOR_TRANSFER_SDR_VIDEO. */
        case AVCOL_TRC_BT709:     /* 1 */
        case AVCOL_TRC_GAMMA22:   /* 4 */
        case AVCOL_TRC_GAMMA28:   /* 5 */
        case AVCOL_TRC_SMPTE170M: /* 6, BT601 */
        case AVCOL_TRC_SMPTE240M: /* 7 */
        case AVCOL_TRC_BT2020_10: /* 14 */
        case AVCOL_TRC_BT2020_12: /* 15 */
            format_color_transfer = 3; /* COLOR_TRANSFER_SDR_VIDEO */
            break;
        default: /* conservative: leave the key unset */
            break;
        }

        switch (avctx->color_primaries) {
        case AVCOL_PRI_BT2020: /* 9 */
            format_color_standard = 6; /* COLOR_STANDARD_BT2020 */
            break;
        case AVCOL_PRI_BT709:  /* 1 */
            format_color_standard = 1; /* COLOR_STANDARD_BT709 */
            break;
        default: /* conservative: leave the key unset */
            break;
        }

        switch (avctx->color_range) {
        case AVCOL_RANGE_JPEG: /* 2, full range */
            format_color_range = 1; /* COLOR_RANGE_FULL */
            break;
        case AVCOL_RANGE_MPEG: /* 1, limited range */
            format_color_range = 2; /* COLOR_RANGE_LIMITED */
            break;
        default: /* AVCOL_RANGE_UNSPECIFIED: leave the key unset */
            break;
        }

        if (format_color_transfer >= 0)
            ff_AMediaFormat_setInt32(format, "color-transfer", format_color_transfer);
        if (format_color_standard >= 0)
            ff_AMediaFormat_setInt32(format, "color-standard", format_color_standard);
        if (format_color_range >= 0)
            ff_AMediaFormat_setInt32(format, "color-range", format_color_range);
    }

    status = ff_AMediaCodec_configure(s->codec, format, s->surface, NULL, 0);
    if (status < 0) {
        char *desc = ff_AMediaFormat_toString(format);
        av_log(avctx, AV_LOG_ERROR,
            "Failed to configure codec %s (status = %d) with format %s\n",
            s->codec_name, status, desc);
        av_freep(&desc);

        ret = AVERROR_EXTERNAL;
        goto fail;
    }

    status = ff_AMediaCodec_start(s->codec);
    if (status < 0) {
        char *desc = ff_AMediaFormat_toString(format);
        av_log(avctx, AV_LOG_ERROR,
            "Failed to start codec %s (status = %d) with format %s\n",
            s->codec_name, status, desc);
        av_freep(&desc);
        ret = AVERROR_EXTERNAL;
        goto fail;
    }

    if (avctx->codec_type == AVMEDIA_TYPE_VIDEO) {
        s->format = ff_AMediaCodec_getOutputFormat(s->codec);
        if (s->format) {
            if ((ret = mediacodec_dec_parse_format(avctx, s)) < 0) {
                av_log(avctx, AV_LOG_ERROR,
                    "Failed to configure context\n");
                goto fail;
            }
        }
    }

    av_log(avctx, AV_LOG_DEBUG, "MediaCodec %p started successfully\n", s->codec);

    return 0;

fail:
    if (s->native_dv_diag.requested)
        av_log(avctx, AV_LOG_INFO,
               "native_dv_diag decoder=%"PRIu64" event=init_failed status=%d\n",
               s->native_dv_diag.decoder_id, ret);
    av_log(avctx, AV_LOG_ERROR, "MediaCodec %p failed to start\n", s->codec);
    ff_mediacodec_dec_close(avctx, s);
    return ret;
}

int64_t ff_mediacodec_pts_to_us(const AVCodecContext *avctx, int64_t pts)
{
    int64_t us = pts == AV_NOPTS_VALUE ? 0 : pts;

    if (us && avctx->pkt_timebase.num && avctx->pkt_timebase.den)
        us = av_rescale_q(us, avctx->pkt_timebase, AV_TIME_BASE_Q);
    return us;
}

int ff_mediacodec_dec_send(AVCodecContext *avctx, MediaCodecDecContext *s,
                           AVPacket *pkt, bool wait)
{
    int offset = 0;
    int diag_captured;
    size_t diag_capacity;
    int need_draining = 0;
    uint8_t *data;
    size_t size;
    FFAMediaCodec *codec = s->codec;
    int status;
    int64_t input_dequeue_timeout_us = wait ? INPUT_DEQUEUE_TIMEOUT_US : 0;
    int64_t pts;

    if (s->flushing) {
        av_log(avctx, AV_LOG_ERROR, "Decoder is flushing and cannot accept new buffer "
                                    "until all output buffers have been released\n");
        return AVERROR_EXTERNAL;
    }

    if (pkt->size == 0) {
        need_draining = 1;
    }

    if (s->draining && s->eos) {
        return AVERROR_EOF;
    }

    while (offset < pkt->size || (need_draining && !s->draining)) {
        ssize_t index = s->current_input_buffer;
        if (index < 0) {
            index = ff_AMediaCodec_dequeueInputBuffer(codec, input_dequeue_timeout_us);
            if (ff_AMediaCodec_infoTryAgainLater(codec, index)) {
                av_log(avctx, AV_LOG_TRACE, "No input buffer available, try again later\n");
                break;
            }

            if (index < 0) {
                av_log(avctx, AV_LOG_ERROR, "Failed to dequeue input buffer (status=%zd)\n", index);
                return AVERROR_EXTERNAL;
            }
        }
        s->current_input_buffer = -1;

        data = ff_AMediaCodec_getInputBuffer(codec, index, &size);
        if (!data) {
            av_log(avctx, AV_LOG_ERROR, "Failed to get input buffer\n");
            return AVERROR_EXTERNAL;
        }

        if (pkt->pts == AV_NOPTS_VALUE)
            av_log(avctx, AV_LOG_WARNING, "Input packet is missing PTS\n");
        pts = ff_mediacodec_pts_to_us(avctx, pkt->pts);

        if (need_draining) {
            uint32_t flags = ff_AMediaCodec_getBufferFlagEndOfStream(codec);

            av_log(avctx, AV_LOG_DEBUG, "Sending End Of Stream signal\n");

            diag_captured = native_dv_diag_queue(avctx, &s->native_dv_diag,
                                                 size, NULL, 0, pts, flags, 1);
            status = ff_AMediaCodec_queueInputBuffer(codec, index, 0, 0, pts, flags);
            native_dv_diag_queue_result(avctx, &s->native_dv_diag,
                                        diag_captured, 0, status, 1);
            if (status < 0) {
                av_log(avctx, AV_LOG_ERROR, "Failed to queue input empty buffer (status = %d)\n", status);
                return AVERROR_EXTERNAL;
            }

            av_log(avctx, AV_LOG_TRACE,
                   "Queued empty EOS input buffer %zd with flags=%d\n", index, flags);

            s->draining = 1;
            return 0;
        }

        diag_capacity = size;
        size = FFMIN(pkt->size - offset, size);
        memcpy(data, pkt->data + offset, size);
        offset += size;

        diag_captured = native_dv_diag_queue(avctx, &s->native_dv_diag,
                                             diag_capacity, data, size, pts, 0, 0);
        status = ff_AMediaCodec_queueInputBuffer(codec, index, 0, size, pts, 0);
        native_dv_diag_queue_result(avctx, &s->native_dv_diag,
                                    diag_captured, size, status, 0);
        if (status < 0) {
            av_log(avctx, AV_LOG_ERROR, "Failed to queue input buffer (status = %d)\n", status);
            return AVERROR_EXTERNAL;
        }

        av_log(avctx, AV_LOG_TRACE,
               "Queued input buffer %zd size=%zd ts=%"PRIi64"\n", index, size, pts);
    }

    if (offset == 0)
        return AVERROR(EAGAIN);
    return offset;
}

int ff_mediacodec_dec_receive(AVCodecContext *avctx, MediaCodecDecContext *s,
                              AVFrame *frame, bool wait, int64_t *out_pts_us)
{
    int ret;
    uint8_t *data;
    ssize_t index;
    size_t size;
    FFAMediaCodec *codec = s->codec;
    FFAMediaCodecBufferInfo info = { 0 };
    int status;
    int64_t output_dequeue_timeout_us = OUTPUT_DEQUEUE_TIMEOUT_US;

    if (out_pts_us)
        *out_pts_us = AV_NOPTS_VALUE;

    if (s->draining && s->eos) {
        return AVERROR_EOF;
    }

    if (s->draining) {
        /* If the codec is flushing or need to be flushed, block for a fair
         * amount of time to ensure we got a frame */
        output_dequeue_timeout_us = OUTPUT_DEQUEUE_BLOCK_TIMEOUT_US;
    } else if (s->output_buffer_count == 0 || !wait) {
        /* If the codec hasn't produced any frames, do not block so we
         * can push data to it as fast as possible, and get the first
         * frame */
        output_dequeue_timeout_us = 0;
    }

    index = ff_AMediaCodec_dequeueOutputBuffer(codec, &info, output_dequeue_timeout_us);
    if (index >= 0) {
        av_log(avctx, AV_LOG_TRACE, "Got output buffer %zd"
                " offset=%" PRIi32 " size=%" PRIi32 " ts=%" PRIi64
                " flags=%" PRIu32 "\n", index, info.offset, info.size,
                info.presentationTimeUs, info.flags);

        if (info.flags & ff_AMediaCodec_getBufferFlagEndOfStream(codec)) {
            s->eos = 1;
        }

        if (info.size) {
            if (out_pts_us)
                *out_pts_us = info.presentationTimeUs;
            if (s->surface) {
                if ((ret = mediacodec_wrap_hw_buffer(avctx, s, index, &info, frame)) < 0) {
                    av_log(avctx, AV_LOG_ERROR, "Failed to wrap MediaCodec buffer\n");
                    return ret;
                }
            } else {
                data = ff_AMediaCodec_getOutputBuffer(codec, index, &size);
                if (!data) {
                    av_log(avctx, AV_LOG_ERROR, "Failed to get output buffer\n");
                    return AVERROR_EXTERNAL;
                }

                if ((ret = mediacodec_wrap_sw_buffer(avctx, s, data, size, index, &info, frame)) < 0) {
                    av_log(avctx, AV_LOG_ERROR, "Failed to wrap MediaCodec buffer\n");
                    return ret;
                }
            }

            s->output_buffer_count++;
            return 0;
        } else {
            status = ff_AMediaCodec_releaseOutputBuffer(codec, index, 0);
            if (status < 0) {
                av_log(avctx, AV_LOG_ERROR, "Failed to release output buffer\n");
            }
        }

    } else if (ff_AMediaCodec_infoOutputFormatChanged(codec, index)) {
        char *format = NULL;

        if (s->format) {
            status = ff_AMediaFormat_delete(s->format);
            if (status < 0) {
                av_log(avctx, AV_LOG_ERROR, "Failed to delete MediaFormat %p\n", s->format);
            }
        }

        s->format = ff_AMediaCodec_getOutputFormat(codec);
        if (!s->format) {
            av_log(avctx, AV_LOG_ERROR, "Failed to get output format\n");
            return AVERROR_EXTERNAL;
        }

        format = ff_AMediaFormat_toString(s->format);
        if (!format) {
            return AVERROR_EXTERNAL;
        }
        av_log(avctx, AV_LOG_INFO, "Output MediaFormat changed to %s\n", format);
        av_freep(&format);

        if ((ret = mediacodec_dec_parse_format(avctx, s)) < 0) {
            return ret;
        }

    } else if (ff_AMediaCodec_infoOutputBuffersChanged(codec, index)) {
        ff_AMediaCodec_cleanOutputBuffers(codec);
    } else if (ff_AMediaCodec_infoTryAgainLater(codec, index)) {
        if (s->draining) {
            av_log(avctx, AV_LOG_ERROR, "Failed to dequeue output buffer within %" PRIi64 "ms "
                                        "while draining remaining frames, output will probably lack frames\n",
                                        output_dequeue_timeout_us / 1000);
        } else {
            av_log(avctx, AV_LOG_TRACE, "No output buffer available, try again later\n");
        }
    } else {
        av_log(avctx, AV_LOG_ERROR, "Failed to dequeue output buffer (status=%zd)\n", index);
        return AVERROR_EXTERNAL;
    }

    if (s->draining && s->eos)
        return AVERROR_EOF;
    return AVERROR(EAGAIN);
}

/*
* ff_mediacodec_dec_flush returns 0 if the flush cannot be performed on
* the codec (because the user retains frames). The codec stays in the
* flushing state.
*
* ff_mediacodec_dec_flush returns 1 if the flush can actually be
* performed on the codec. The codec leaves the flushing state and can
* process again packets.
*
* ff_mediacodec_dec_flush returns a negative value if an error has
* occurred.
*/
int ff_mediacodec_dec_flush(AVCodecContext *avctx, MediaCodecDecContext *s)
{
    if (!s->surface || !s->delay_flush || atomic_load(&s->refcount) == 1) {
        int ret;

        /* No frames (holding a reference to the codec) are retained by the
         * user, thus we can flush the codec and returns accordingly */
        if ((ret = mediacodec_dec_flush_codec(avctx, s)) < 0) {
            return ret;
        }

        return 1;
    }

    s->flushing = 1;
    return 0;
}

int ff_mediacodec_dec_close(AVCodecContext *avctx, MediaCodecDecContext *s)
{
    if (!s)
        return 0;

    ff_mediacodec_diag_close(avctx, &s->native_dv_diag);

    if (s->codec) {
        if (atomic_load(&s->hw_buffer_count) == 0) {
            ff_AMediaCodec_stop(s->codec);
            av_log(avctx, AV_LOG_DEBUG, "MediaCodec %p stopped\n", s->codec);
        } else {
            av_log(avctx, AV_LOG_DEBUG, "Not stopping MediaCodec (there are buffers pending)\n");
        }
    }

    ff_mediacodec_dec_unref(s);

    return 0;
}

int ff_mediacodec_dec_is_flushing(AVCodecContext *avctx, MediaCodecDecContext *s)
{
    return s->flushing;
}
