/*
 * Android MediaCodec MPEG-2 / H.264 / H.265 / MPEG-4 / VP8 / VP9 decoders
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

#include "config_components.h"

#include <stdint.h>
#include <string.h>

#include "libavutil/avassert.h"
#include "libavutil/common.h"
#include "libavutil/mem.h"
#include "libavutil/opt.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/pixfmt.h"
#include "libavutil/internal.h"

#include "avcodec.h"
#include "codec_internal.h"
#include "decode.h"
#include "dovi_rpu.h"
#include "h2645_parse.h"
#include "h264_parse.h"
#include "h264_ps.h"
#include "hevc/parse.h"
#include "hwconfig.h"
#include "internal.h"
#include "jni.h"
#include "mediacodec_wrapper.h"
#include "mediacodecdec_common.h"

typedef struct P5RpuEntry {
    unsigned seq;
    int valid;
    int duplicate_pts;
    int64_t pts_us;
    uint32_t rpu_hash;
    unsigned rpu_size;
    unsigned rpu_count;
    int missing_pts;
    int parse_error;
    int consumed;
    unsigned epoch;
    AVDOVIMetadata *metadata;
    int metadata_size;
    uint8_t *raw_rpu;
    int raw_rpu_size;
} P5RpuEntry;

typedef struct MediaCodecH264DecContext {

    AVClass *avclass;

    MediaCodecDecContext *ctx;

    AVPacket buffered_pkt;

    int delay_flush;
    int amlogic_mpeg2_api23_workaround;

    int use_ndk_codec;

    /* Match P5 access-unit RPU metadata to MediaCodec output by timestamp. */
    struct {
        int enabled;
        int attach;
        DOVIContext dovi;
        unsigned epoch;
        unsigned inputs;
        unsigned outputs;
        unsigned matched;
        unsigned errors;
        unsigned discarded;
        int pending;
        P5RpuEntry entries[320], next;
    } p5_rpu;
} MediaCodecH264DecContext;

static void p5_rpu_clear_entry(P5RpuEntry *entry)
{
    av_freep(&entry->metadata);
    av_freep(&entry->raw_rpu);
    entry->metadata_size = 0;
    entry->raw_rpu_size = 0;
}

static int p5_rpu_start_code(const uint8_t *data, int size, int pos,
                               int *header)
{
    for (int i = pos; i + 3 <= size; i++) {
        if (data[i] || data[i + 1] ||
            (data[i + 2] != 1 &&
             !(i + 3 < size && !data[i + 2] && data[i + 3] == 1)))
            continue;
        *header = i + (data[i + 2] == 1 ? 3 : 4);
        return i;
    }
    return -1;
}

static void p5_rpu_nal(MediaCodecH264DecContext *s,
                         const uint8_t *data, int size)
{
    uint32_t hash;

    if (size < 2) {
        s->p5_rpu.next.parse_error = 1;
        return;
    }
    if (((data[0] >> 1) & 0x3f) != 62)
        return;

    s->p5_rpu.next.rpu_count++;
    /* A Dolby Vision RPU has layer id 0 and temporal_id_plus1 1. */
    if (((data[0] & 1) << 5 | data[1] >> 3) != 0 || (data[1] & 7) != 1)
        s->p5_rpu.next.parse_error = 1;
    s->p5_rpu.next.rpu_size += size - 2;
    hash = s->p5_rpu.next.rpu_hash ? s->p5_rpu.next.rpu_hash : 2166136261U;
    for (int i = 2; i < size; i++)
        hash = (hash ^ data[i]) * 16777619U;
    s->p5_rpu.next.rpu_hash = hash;
}

static void p5_rpu_parse_metadata(AVCodecContext *avctx, AVPacket *pkt,
                                    int length_size)
{
    MediaCodecH264DecContext *s = avctx->priv_data;
    H2645Packet split = { 0 };
    size_t sd_size = 0;
    const uint8_t *sd;
    int flags = H2645_FLAG_SMALL_PADDING;
    int ret;

    if (s->p5_rpu.next.rpu_count != 1 || s->p5_rpu.next.parse_error)
        return;
    sd = av_packet_get_side_data(pkt, AV_PKT_DATA_DOVI_CONF, &sd_size);
    if (sd && sd_size >= sizeof(s->p5_rpu.dovi.cfg))
        memcpy(&s->p5_rpu.dovi.cfg, sd, sizeof(s->p5_rpu.dovi.cfg));
    if (length_size)
        flags |= H2645_FLAG_IS_NALFF;
    ret = ff_h2645_packet_split(&split, pkt->data, pkt->size, avctx,
                                length_size, AV_CODEC_ID_HEVC, flags);
    if (ret < 0) {
        s->p5_rpu.next.parse_error = 1;
        goto done;
    }
    for (int i = 0; i < split.nb_nals; i++) {
        H2645NAL *nal = &split.nals[i];
        if (nal->type != 62)
            continue;
        if (nal->size <= 2 || nal->raw_size <= 2 ||
            nal->nuh_layer_id || nal->temporal_id) {
            s->p5_rpu.next.parse_error = 1;
            break;
        }
        ret = ff_dovi_rpu_parse(&s->p5_rpu.dovi, nal->data + 2,
                                nal->size - 2, avctx->err_recognition);
        if (ret < 0) {
            s->p5_rpu.next.parse_error = 1;
            break;
        }
        s->p5_rpu.next.metadata_size = ff_dovi_get_metadata(
            &s->p5_rpu.dovi, &s->p5_rpu.next.metadata);
        if (s->p5_rpu.next.metadata_size <= 0) {
            s->p5_rpu.next.parse_error = 1;
            break;
        }
        s->p5_rpu.next.raw_rpu_size = nal->raw_size - 2;
        s->p5_rpu.next.raw_rpu = av_memdup(nal->raw_data + 2,
                                              s->p5_rpu.next.raw_rpu_size);
        if (!s->p5_rpu.next.raw_rpu)
            s->p5_rpu.next.parse_error = 1;
        break;
    }
    if (!s->p5_rpu.next.metadata)
        s->p5_rpu.next.parse_error = 1;
done:
    if (s->p5_rpu.next.parse_error)
        p5_rpu_clear_entry(&s->p5_rpu.next);
    ff_h2645_packet_uninit(&split);
}

static void p5_rpu_packet(AVCodecContext *avctx, AVPacket *pkt)
{
    MediaCodecH264DecContext *s = avctx->priv_data;
    int header, start, pos, length_size = 0;

    if (!s->p5_rpu.enabled)
        return;
    p5_rpu_clear_entry(&s->p5_rpu.next);
    memset(&s->p5_rpu.next, 0, sizeof(s->p5_rpu.next));
    s->p5_rpu.pending = 1;
    s->p5_rpu.next.epoch = s->p5_rpu.epoch;
    s->p5_rpu.next.missing_pts = pkt->pts == AV_NOPTS_VALUE;
    s->p5_rpu.next.pts_us = pkt->pts;
    if (!s->p5_rpu.next.missing_pts && pkt->pts &&
        avctx->pkt_timebase.num && avctx->pkt_timebase.den)
        s->p5_rpu.next.pts_us = av_rescale_q(pkt->pts, avctx->pkt_timebase,
                                                AV_TIME_BASE_Q);
    if (!pkt->data || pkt->size <= 0) {
        s->p5_rpu.next.parse_error = 1;
        return;
    }

    start = p5_rpu_start_code(pkt->data, pkt->size, 0, &header);
    if (start >= 0 && start <= 3) {
        while (start >= 0) {
            int next_header, next = p5_rpu_start_code(pkt->data, pkt->size,
                                                         header, &next_header);
            p5_rpu_nal(s, pkt->data + header,
                         (next < 0 ? pkt->size : next) - header);
            start = next;
            if (next >= 0)
                header = next_header;
        }
    } else {
        /* hvcC stores lengthSizeMinusOne in the low two bits of byte 21. */
        if (avctx->extradata_size > 21 && avctx->extradata[0] == 1)
            length_size = (avctx->extradata[21] & 3) + 1;
        if (!length_size) {
            s->p5_rpu.next.parse_error = 1;
            return;
        }
        for (pos = 0; pos < pkt->size;) {
            unsigned len = 0;
            if (pkt->size - pos < length_size) {
                s->p5_rpu.next.parse_error = 1;
                break;
            }
            for (int i = 0; i < length_size; i++)
                len = (len << 8) | pkt->data[pos++];
            if (!len || len > pkt->size - pos) {
                s->p5_rpu.next.parse_error = 1;
                break;
            }
            p5_rpu_nal(s, pkt->data + pos, len);
            pos += len;
        }
    }
    p5_rpu_parse_metadata(avctx, pkt, length_size);
}

static void p5_rpu_commit(AVCodecContext *avctx)
{
    MediaCodecH264DecContext *s = avctx->priv_data;
    P5RpuEntry *entry;
    unsigned seq, slot;
    const char *status = "OK";

    if (!s->p5_rpu.enabled || !s->p5_rpu.pending)
        return;
    s->p5_rpu.pending = 0;
    seq = s->p5_rpu.inputs++;
    slot = seq % FF_ARRAY_ELEMS(s->p5_rpu.entries);
    entry = &s->p5_rpu.entries[slot];
    if (entry->valid && !entry->consumed) {
        s->p5_rpu.discarded++;
        s->p5_rpu.errors++;
        if (s->p5_rpu.discarded <= 20 ||
            s->p5_rpu.discarded % 1000 == 0)
            av_log(avctx, AV_LOG_ERROR,
                   "MEDIA_KIT_P5_RPU stale eviction epoch=%u input_seq=%u held_seq=%u discarded=%u\n",
                   s->p5_rpu.epoch, seq, entry->seq,
                   s->p5_rpu.discarded);
    }
    for (unsigned j = 0; j < FF_ARRAY_ELEMS(s->p5_rpu.entries); j++) {
        P5RpuEntry *old = &s->p5_rpu.entries[j];
        if (old->valid && old->epoch == s->p5_rpu.epoch &&
            !old->missing_pts && !s->p5_rpu.next.missing_pts &&
            old->pts_us == s->p5_rpu.next.pts_us)
            s->p5_rpu.next.duplicate_pts = 1;
    }
    p5_rpu_clear_entry(entry);
    *entry = s->p5_rpu.next;
    entry->seq = seq;
    entry->valid = 1;
    s->p5_rpu.next.metadata = NULL;
    s->p5_rpu.next.raw_rpu = NULL;
    if (entry->missing_pts)
        status = "MISSING_INPUT_PTS";
    else if (entry->parse_error)
        status = "PARSE_ERROR";
    else if (entry->rpu_count != 1)
        status = entry->rpu_count ? "MULTIPLE_RPU" : "MISSING_RPU";
    else if (entry->duplicate_pts)
        status = "DUPLICATE_INPUT_PTS";
    if (strcmp(status, "OK"))
        s->p5_rpu.errors++;
    if (strcmp(status, "OK") && s->p5_rpu.errors <= 20)
        av_log(avctx, AV_LOG_WARNING,
               "MEDIA_KIT_P5_RPU input epoch=%u seq=%u pts_us=%"PRId64
               " rpu=%u bytes=%u hash=%08x meta_bytes=%d profile=%u residual=%d status=%s\n",
               s->p5_rpu.epoch, seq, entry->pts_us,
               entry->rpu_count, entry->rpu_size, entry->rpu_hash,
               entry->metadata_size, s->p5_rpu.dovi.cfg.dv_profile,
               entry->metadata ?
                   av_dovi_get_header(entry->metadata)->disable_residual_flag : -1,
               status);
}

static void p5_rpu_output(AVCodecContext *avctx, AVFrame *frame)
{
    MediaCodecH264DecContext *s = avctx->priv_data;
    P5RpuEntry *entry = NULL;
    int64_t pts_us;
    int found = -1, count = 0, attached = 0, metadata_size = 0;
    const char *status;

    if (!s->p5_rpu.enabled)
        return;
    if (frame->format == AV_PIX_FMT_MEDIACODEC && frame->data[3])
        pts_us = ((MediaCodecBuffer *)frame->data[3])->pts;
    else if (frame->pts != AV_NOPTS_VALUE && avctx->pkt_timebase.num &&
             avctx->pkt_timebase.den)
        pts_us = av_rescale_q(frame->pts, avctx->pkt_timebase, AV_TIME_BASE_Q);
    else
        pts_us = AV_NOPTS_VALUE;

    s->p5_rpu.outputs++;
    for (unsigned i = 0; i < FF_ARRAY_ELEMS(s->p5_rpu.entries); i++) {
        if (s->p5_rpu.entries[i].valid &&
            s->p5_rpu.entries[i].epoch == s->p5_rpu.epoch &&
            !s->p5_rpu.entries[i].missing_pts && pts_us != AV_NOPTS_VALUE &&
            s->p5_rpu.entries[i].pts_us == pts_us) {
            found = i;
            count++;
        }
    }
    if (pts_us == AV_NOPTS_VALUE)
        status = "MISSING_OUTPUT_PTS";
    else if (count > 1)
        status = "DUPLICATE_INPUT_PTS";
    else if (count == 0)
        status = "UNMATCHED_OUTPUT_PTS";
    else if (s->p5_rpu.entries[found].consumed)
        status = "DUPLICATE_OUTPUT_PTS";
    else if (s->p5_rpu.entries[found].duplicate_pts)
        status = "DUPLICATE_INPUT_PTS";
    else if (s->p5_rpu.entries[found].parse_error)
        status = "PARSE_ERROR";
    else if (s->p5_rpu.entries[found].rpu_count != 1)
        status = s->p5_rpu.entries[found].rpu_count ? "MULTIPLE_RPU" : "MISSING_RPU";
    else
        status = "MATCH";

    if (count > 1) {
        for (unsigned i = 0; i < FF_ARRAY_ELEMS(s->p5_rpu.entries); i++) {
            P5RpuEntry *ambiguous = &s->p5_rpu.entries[i];
            if (ambiguous->valid && ambiguous->epoch == s->p5_rpu.epoch &&
                !ambiguous->missing_pts && ambiguous->pts_us == pts_us) {
                ambiguous->consumed = 1;
                p5_rpu_clear_entry(ambiguous);
            }
        }
    }
    if (count == 1 && !s->p5_rpu.entries[found].consumed) {
        AVFrameSideData *meta_sd, *raw_sd;
        entry = &s->p5_rpu.entries[found];
        metadata_size = entry->metadata_size;
        if (s->p5_rpu.attach && !strcmp(status, "MATCH")) {
            if (!entry->metadata || !entry->raw_rpu) {
                status = "MISSING_PARSED_METADATA";
            } else if (av_frame_get_side_data(frame, AV_FRAME_DATA_DOVI_METADATA) ||
                       av_frame_get_side_data(frame, AV_FRAME_DATA_DOVI_RPU_BUFFER)) {
                status = "EXISTING_DOVI_SIDE_DATA";
            } else {
                meta_sd = av_frame_new_side_data(frame, AV_FRAME_DATA_DOVI_METADATA,
                                                 entry->metadata_size);
                raw_sd = meta_sd ? av_frame_new_side_data(frame, AV_FRAME_DATA_DOVI_RPU_BUFFER,
                                                          entry->raw_rpu_size) : NULL;
                if (!meta_sd || !raw_sd) {
                    if (meta_sd)
                        av_frame_remove_side_data(frame, AV_FRAME_DATA_DOVI_METADATA);
                    status = "ATTACH_ERROR";
                } else {
                    memcpy(meta_sd->data, entry->metadata, entry->metadata_size);
                    memcpy(raw_sd->data, entry->raw_rpu, entry->raw_rpu_size);
                    attached = 1;
                }
            }
        }
        entry->consumed = 1;
        p5_rpu_clear_entry(entry);
    }
    if (!strcmp(status, "MATCH"))
        s->p5_rpu.matched++;
    if (strcmp(status, "MATCH"))
        s->p5_rpu.errors++;
    if (strcmp(status, "MATCH") && s->p5_rpu.errors <= 20)
        av_log(avctx, AV_LOG_WARNING,
               "MEDIA_KIT_P5_RPU output epoch=%u seq=%u pts_us=%"PRId64
               " input_seq=%d meta_bytes=%d attached=%d status=%s\n", s->p5_rpu.epoch,
               s->p5_rpu.outputs - 1, pts_us,
               count == 1 ? (int)s->p5_rpu.entries[found].seq : -1,
               metadata_size, attached, status);
}

static int p5_rpu_receive(AVCodecContext *avctx, AVFrame *frame, bool wait)
{
    MediaCodecH264DecContext *s = avctx->priv_data;
    int ret = ff_mediacodec_dec_receive(avctx, s->ctx, frame, wait);
    if (!ret)
        p5_rpu_output(avctx, frame);
    return ret;
}

static void p5_rpu_flush(AVCodecContext *avctx)
{
    MediaCodecH264DecContext *s = avctx->priv_data;
    if (!s->p5_rpu.enabled)
        return;
    for (unsigned i = 0; i < FF_ARRAY_ELEMS(s->p5_rpu.entries); i++) {
        if (s->p5_rpu.entries[i].valid &&
            s->p5_rpu.entries[i].epoch == s->p5_rpu.epoch &&
            !s->p5_rpu.entries[i].consumed)
            s->p5_rpu.discarded++;
        p5_rpu_clear_entry(&s->p5_rpu.entries[i]);
        s->p5_rpu.entries[i].valid = 0;
    }
    av_log(avctx, AV_LOG_WARNING,
           "MEDIA_KIT_P5_RPU flush epoch=%u next_epoch=%u discarded=%u\n",
           s->p5_rpu.epoch, s->p5_rpu.epoch + 1, s->p5_rpu.discarded);
    p5_rpu_clear_entry(&s->p5_rpu.next);
    s->p5_rpu.pending = 0;
    ff_dovi_ctx_flush(&s->p5_rpu.dovi);
    s->p5_rpu.epoch++;
}

static av_cold int mediacodec_decode_close(AVCodecContext *avctx)
{
    MediaCodecH264DecContext *s = avctx->priv_data;
    unsigned unconsumed = 0;

    if (s->p5_rpu.enabled) {
        for (unsigned i = 0; i < FF_ARRAY_ELEMS(s->p5_rpu.entries); i++)
            if (s->p5_rpu.entries[i].valid &&
                !s->p5_rpu.entries[i].consumed)
                unconsumed++;
        av_log(avctx, AV_LOG_WARNING,
               "MEDIA_KIT_P5_RPU summary inputs=%u outputs=%u matched=%u errors=%u discarded=%u unconsumed=%u epoch=%u\n",
               s->p5_rpu.inputs, s->p5_rpu.outputs, s->p5_rpu.matched,
               s->p5_rpu.errors, s->p5_rpu.discarded, unconsumed,
               s->p5_rpu.epoch);
        for (unsigned i = 0; i < FF_ARRAY_ELEMS(s->p5_rpu.entries); i++)
            p5_rpu_clear_entry(&s->p5_rpu.entries[i]);
        p5_rpu_clear_entry(&s->p5_rpu.next);
        ff_dovi_ctx_unref(&s->p5_rpu.dovi);
    }

    ff_mediacodec_dec_close(avctx, s->ctx);
    s->ctx = NULL;

    av_packet_unref(&s->buffered_pkt);

    return 0;
}

#if CONFIG_H264_MEDIACODEC_DECODER || CONFIG_HEVC_MEDIACODEC_DECODER
static int h2645_ps_to_nalu(const uint8_t *src, int src_size, uint8_t **out, int *out_size)
{
    int i;
    int ret = 0;
    uint8_t *p = NULL;
    static const uint8_t nalu_header[] = { 0x00, 0x00, 0x00, 0x01 };

    if (!out || !out_size) {
        return AVERROR(EINVAL);
    }

    p = av_malloc(sizeof(nalu_header) + src_size);
    if (!p) {
        return AVERROR(ENOMEM);
    }

    *out = p;
    *out_size = sizeof(nalu_header) + src_size;

    memcpy(p, nalu_header, sizeof(nalu_header));
    memcpy(p + sizeof(nalu_header), src, src_size);

    /* Escape 0x00, 0x00, 0x0{0-3} pattern */
    for (i = 4; i < *out_size; i++) {
        if (i < *out_size - 3 &&
            p[i + 0] == 0 &&
            p[i + 1] == 0 &&
            p[i + 2] <= 3) {
            uint8_t *new;

            *out_size += 1;
            new = av_realloc(*out, *out_size);
            if (!new) {
                ret = AVERROR(ENOMEM);
                goto done;
            }
            *out = p = new;

            i = i + 2;
            memmove(p + i + 1, p + i, *out_size - (i + 1));
            p[i] = 0x03;
        }
    }
done:
    if (ret < 0) {
        av_freep(out);
        *out_size = 0;
    }

    return ret;
}
#endif

#if CONFIG_H264_MEDIACODEC_DECODER
static int h264_set_extradata(AVCodecContext *avctx, FFAMediaFormat *format)
{
    int i;
    int ret;

    H264ParamSets ps;
    const PPS *pps = NULL;
    const SPS *sps = NULL;
    int is_avc = 0;
    int nal_length_size = 0;

    memset(&ps, 0, sizeof(ps));

    ret = ff_h264_decode_extradata(avctx->extradata, avctx->extradata_size,
                                   &ps, &is_avc, &nal_length_size, 0, avctx);
    if (ret < 0) {
        goto done;
    }

    for (i = 0; i < MAX_PPS_COUNT; i++) {
        if (ps.pps_list[i]) {
            pps = ps.pps_list[i];
            break;
        }
    }

    if (pps) {
        if (ps.sps_list[pps->sps_id]) {
            sps = ps.sps_list[pps->sps_id];
        }
    }

    if (pps && sps) {
        uint8_t *data = NULL;
        int data_size = 0;

        avctx->profile = ff_h264_get_profile(sps);
        avctx->level = sps->level_idc;

        if ((ret = h2645_ps_to_nalu(sps->data, sps->data_size, &data, &data_size)) < 0) {
            goto done;
        }
        ff_AMediaFormat_setBuffer(format, "csd-0", (void*)data, data_size);
        av_freep(&data);

        if ((ret = h2645_ps_to_nalu(pps->data, pps->data_size, &data, &data_size)) < 0) {
            goto done;
        }
        ff_AMediaFormat_setBuffer(format, "csd-1", (void*)data, data_size);
        av_freep(&data);
    } else {
        const int warn = is_avc && (avctx->codec_tag == MKTAG('a','v','c','1') ||
                                    avctx->codec_tag == MKTAG('a','v','c','2'));
        av_log(avctx, warn ? AV_LOG_WARNING : AV_LOG_DEBUG,
               "Could not extract PPS/SPS from extradata\n");
        ret = 0;
    }

done:
    ff_h264_ps_uninit(&ps);

    return ret;
}
#endif

#if CONFIG_HEVC_MEDIACODEC_DECODER
static int hevc_set_extradata(AVCodecContext *avctx, FFAMediaFormat *format)
{
    int i;
    int ret;

    HEVCParamSets ps;
    HEVCSEI sei;

    const HEVCVPS *vps = NULL;
    const HEVCPPS *pps = NULL;
    const HEVCSPS *sps = NULL;
    int is_nalff = 0;
    int nal_length_size = 0;

    uint8_t *vps_data = NULL;
    uint8_t *sps_data = NULL;
    uint8_t *pps_data = NULL;
    int vps_data_size = 0;
    int sps_data_size = 0;
    int pps_data_size = 0;

    memset(&ps, 0, sizeof(ps));
    memset(&sei, 0, sizeof(sei));

    ret = ff_hevc_decode_extradata(avctx->extradata, avctx->extradata_size,
                                   &ps, &sei, &is_nalff, &nal_length_size, 0, 1, avctx);
    if (ret < 0) {
        goto done;
    }

    for (i = 0; i < HEVC_MAX_VPS_COUNT; i++) {
        if (ps.vps_list[i]) {
            vps = ps.vps_list[i];
            break;
        }
    }

    for (i = 0; i < HEVC_MAX_PPS_COUNT; i++) {
        if (ps.pps_list[i]) {
            pps = ps.pps_list[i];
            break;
        }
    }

    if (pps) {
        if (ps.sps_list[pps->sps_id]) {
            sps = ps.sps_list[pps->sps_id];
        }
    }

    if (vps && pps && sps) {
        uint8_t *data;
        int data_size;

        avctx->profile = sps->ptl.general_ptl.profile_idc;
        avctx->level   = sps->ptl.general_ptl.level_idc;

        if ((ret = h2645_ps_to_nalu(vps->data, vps->data_size, &vps_data, &vps_data_size)) < 0 ||
            (ret = h2645_ps_to_nalu(sps->data, sps->data_size, &sps_data, &sps_data_size)) < 0 ||
            (ret = h2645_ps_to_nalu(pps->data, pps->data_size, &pps_data, &pps_data_size)) < 0) {
            goto done;
        }

        data_size = vps_data_size + sps_data_size + pps_data_size;
        data = av_mallocz(data_size);
        if (!data) {
            ret = AVERROR(ENOMEM);
            goto done;
        }

        memcpy(data                                , vps_data, vps_data_size);
        memcpy(data + vps_data_size                , sps_data, sps_data_size);
        memcpy(data + vps_data_size + sps_data_size, pps_data, pps_data_size);

        ff_AMediaFormat_setBuffer(format, "csd-0", data, data_size);

        av_freep(&data);
    } else {
        const int warn = is_nalff && avctx->codec_tag == MKTAG('h','v','c','1');
        av_log(avctx, warn ? AV_LOG_WARNING : AV_LOG_DEBUG,
               "Could not extract VPS/PPS/SPS from extradata\n");
        ret = 0;
    }

done:
    ff_hevc_ps_uninit(&ps);

    av_freep(&vps_data);
    av_freep(&sps_data);
    av_freep(&pps_data);

    return ret;
}
#endif

#if CONFIG_MPEG2_MEDIACODEC_DECODER || \
    CONFIG_MPEG4_MEDIACODEC_DECODER || \
    CONFIG_VP8_MEDIACODEC_DECODER   || \
    CONFIG_VP9_MEDIACODEC_DECODER   || \
    CONFIG_AV1_MEDIACODEC_DECODER   || \
    CONFIG_AAC_MEDIACODEC_DECODER   || \
    CONFIG_AMRNB_MEDIACODEC_DECODER || \
    CONFIG_AMRWB_MEDIACODEC_DECODER || \
    CONFIG_MP3_MEDIACODEC_DECODER
static int common_set_extradata(AVCodecContext *avctx, FFAMediaFormat *format)
{
    int ret = 0;

    if (avctx->extradata) {
        ff_AMediaFormat_setBuffer(format, "csd-0", avctx->extradata, avctx->extradata_size);
    }

    return ret;
}
#endif

static av_cold int mediacodec_decode_init(AVCodecContext *avctx)
{
    int ret;
    int sdk_int;

    const char *codec_mime = NULL;

    FFAMediaFormat *format = NULL;
    MediaCodecH264DecContext *s = avctx->priv_data;

#if defined(__ANDROID__)
    if (avctx->codec_id == AV_CODEC_ID_HEVC) {
        const AVPacketSideData *sd =
            ff_get_coded_side_data(avctx, AV_PKT_DATA_DOVI_CONF);
        if (sd && sd->size >= sizeof(s->p5_rpu.dovi.cfg)) {
            memcpy(&s->p5_rpu.dovi.cfg, sd->data,
                   sizeof(s->p5_rpu.dovi.cfg));
            if (s->p5_rpu.dovi.cfg.dv_profile == 5)
                s->p5_rpu.enabled = s->p5_rpu.attach = 1;
        }
        if (s->p5_rpu.enabled) {
            s->p5_rpu.dovi.logctx = avctx;
        }
    }
#endif

    if (s->use_ndk_codec < 0)
        s->use_ndk_codec = !av_jni_get_java_vm(avctx);

    format = ff_AMediaFormat_new(s->use_ndk_codec);
    if (!format) {
        av_log(avctx, AV_LOG_ERROR, "Failed to create media format\n");
        ret = AVERROR_EXTERNAL;
        goto done;
    }

    switch (avctx->codec_id) {
#if CONFIG_AV1_MEDIACODEC_DECODER
    case AV_CODEC_ID_AV1:
        codec_mime = "video/av01";

        ret = common_set_extradata(avctx, format);
        if (ret < 0)
            goto done;
        break;
#endif
#if CONFIG_H264_MEDIACODEC_DECODER
    case AV_CODEC_ID_H264:
        codec_mime = "video/avc";

        ret = h264_set_extradata(avctx, format);
        if (ret < 0)
            goto done;
        break;
#endif
#if CONFIG_HEVC_MEDIACODEC_DECODER
    case AV_CODEC_ID_HEVC:
        codec_mime = "video/hevc";

        ret = hevc_set_extradata(avctx, format);
        if (ret < 0)
            goto done;
        break;
#endif
#if CONFIG_MPEG2_MEDIACODEC_DECODER
    case AV_CODEC_ID_MPEG2VIDEO:
        codec_mime = "video/mpeg2";

        ret = common_set_extradata(avctx, format);
        if (ret < 0)
            goto done;
        break;
#endif
#if CONFIG_MPEG4_MEDIACODEC_DECODER
    case AV_CODEC_ID_MPEG4:
        codec_mime = "video/mp4v-es",

        ret = common_set_extradata(avctx, format);
        if (ret < 0)
            goto done;
        break;
#endif
#if CONFIG_VP8_MEDIACODEC_DECODER
    case AV_CODEC_ID_VP8:
        codec_mime = "video/x-vnd.on2.vp8";

        ret = common_set_extradata(avctx, format);
        if (ret < 0)
            goto done;
        break;
#endif
#if CONFIG_VP9_MEDIACODEC_DECODER
    case AV_CODEC_ID_VP9:
        codec_mime = "video/x-vnd.on2.vp9";

        ret = common_set_extradata(avctx, format);
        if (ret < 0)
            goto done;
        break;
#endif
#if CONFIG_AAC_MEDIACODEC_DECODER
    case AV_CODEC_ID_AAC:
        codec_mime = "audio/mp4a-latm";

        ret = common_set_extradata(avctx, format);
        if (ret < 0)
            goto done;
        break;
#endif
#if CONFIG_AMRNB_MEDIACODEC_DECODER
    case AV_CODEC_ID_AMR_NB:
        codec_mime = "audio/3gpp";

        ret = common_set_extradata(avctx, format);
        if (ret < 0)
            goto done;
        break;
#endif
#if CONFIG_AMRWB_MEDIACODEC_DECODER
    case AV_CODEC_ID_AMR_WB:
        codec_mime = "audio/amr-wb";

        ret = common_set_extradata(avctx, format);
        if (ret < 0)
            goto done;
        break;
#endif
#if CONFIG_MP3_MEDIACODEC_DECODER
    case AV_CODEC_ID_MP3:
        codec_mime = "audio/mpeg";

        ret = common_set_extradata(avctx, format);
        if (ret < 0)
            goto done;
        break;
#endif
    default:
        av_assert0(0);
    }

    ff_AMediaFormat_setString(format, "mime", codec_mime);

    if (avctx->codec_type == AVMEDIA_TYPE_VIDEO) {
        ff_AMediaFormat_setInt32(format, "width", avctx->width);
        ff_AMediaFormat_setInt32(format, "height", avctx->height);
    } else {
        ff_AMediaFormat_setInt32(format, "channel-count", avctx->ch_layout.nb_channels);
        ff_AMediaFormat_setInt32(format, "sample-rate", avctx->sample_rate);
    }

    s->ctx = av_mallocz(sizeof(*s->ctx));
    if (!s->ctx) {
        av_log(avctx, AV_LOG_ERROR, "Failed to allocate MediaCodecDecContext\n");
        ret = AVERROR(ENOMEM);
        goto done;
    }

    s->ctx->delay_flush = s->delay_flush;
    s->ctx->use_ndk_codec = s->use_ndk_codec;

    if ((ret = ff_mediacodec_dec_init(avctx, s->ctx, codec_mime, format)) < 0) {
        s->ctx = NULL;
        goto done;
    }

    av_log(avctx, AV_LOG_INFO,
           "MediaCodec started successfully: codec = %s, ret = %d\n",
           s->ctx->codec_name, ret);

    sdk_int = ff_Build_SDK_INT(avctx);
    /* ff_Build_SDK_INT can fail when target API < 24 and JVM isn't available.
     * If we don't check sdk_int > 0, the workaround might be enabled by
     * mistake.
     * JVM is required to make the workaround works reliably. On the other hand,
     * missing a workaround should not be a serious issue, we do as best we can.
     */
    if (sdk_int > 0 && sdk_int <= 23 &&
        strcmp(s->ctx->codec_name, "OMX.amlogic.mpeg2.decoder.awesome") == 0) {
        av_log(avctx, AV_LOG_INFO, "Enabling workaround for %s on API=%d\n",
               s->ctx->codec_name, sdk_int);
        s->amlogic_mpeg2_api23_workaround = 1;
    }

done:
    if (format) {
        ff_AMediaFormat_delete(format);
    }

    if (ret < 0) {
        mediacodec_decode_close(avctx);
    }

    return ret;
}

static int mediacodec_receive_frame(AVCodecContext *avctx, AVFrame *frame)
{
    MediaCodecH264DecContext *s = avctx->priv_data;
    int ret;
    ssize_t index;

    /* In delay_flush mode, wait until the user has released or rendered
       all retained frames. */
    if (s->delay_flush && ff_mediacodec_dec_is_flushing(avctx, s->ctx)) {
        int flush_ret = ff_mediacodec_dec_flush(avctx, s->ctx);
        if (!flush_ret) {
            return AVERROR(EAGAIN);
        }
        if (flush_ret == 1)
            p5_rpu_flush(avctx);
    }

    /* poll for new frame */
    ret = p5_rpu_receive(avctx, frame, false);
    if (ret != AVERROR(EAGAIN))
        return ret;

    /* feed decoder */
    while (1) {
        if (s->ctx->current_input_buffer < 0 && !s->ctx->draining) {
            /* poll for input space */
            index = ff_AMediaCodec_dequeueInputBuffer(s->ctx->codec, 0);
            if (index < 0) {
                /* no space, block for an output frame to appear */
                ret = p5_rpu_receive(avctx, frame, true);
                /* Try again if both input port and output port return EAGAIN.
                 * If no data is consumed and no frame in output, it can make
                 * both avcodec_send_packet() and avcodec_receive_frame()
                 * return EAGAIN, which violate the design.
                 */
                if (ff_AMediaCodec_infoTryAgainLater(s->ctx->codec, index) &&
                    ret == AVERROR(EAGAIN))
                    continue;
                return ret;
            }
            s->ctx->current_input_buffer = index;
        }

        /* try to flush any buffered packet data */
        if (s->buffered_pkt.size > 0) {
            ret = ff_mediacodec_dec_send(avctx, s->ctx, &s->buffered_pkt, false);
            if (ret >= 0) {
                s->buffered_pkt.size -= ret;
                s->buffered_pkt.data += ret;
                if (s->buffered_pkt.size <= 0) {
                    av_packet_unref(&s->buffered_pkt);
                } else {
                    av_log(avctx, AV_LOG_WARNING,
                           "could not send entire packet in single input buffer (%d < %d)\n",
                           ret, s->buffered_pkt.size+ret);
                }
            } else if (ret < 0 && ret != AVERROR(EAGAIN)) {
                return ret;
            }

            if (s->amlogic_mpeg2_api23_workaround && s->buffered_pkt.size <= 0) {
                /* fallthrough to fetch next packet regardless of input buffer space */
            } else {
                /* poll for space again */
                continue;
            }
        }

        /* fetch new packet or eof */
        ret = ff_decode_get_packet(avctx, &s->buffered_pkt);
        if (ret == AVERROR_EOF) {
            AVPacket null_pkt = { 0 };
            ret = ff_mediacodec_dec_send(avctx, s->ctx, &null_pkt, true);
            if (ret < 0)
                return ret;
            return p5_rpu_receive(avctx, frame, true);
        } else if (ret == AVERROR(EAGAIN) && s->ctx->current_input_buffer < 0) {
            return p5_rpu_receive(avctx, frame, true);
        } else if (ret < 0) {
            return ret;
        }
        p5_rpu_packet(avctx, &s->buffered_pkt);
        /* Register before queueing: a split AU can produce output early. */
        p5_rpu_commit(avctx);
    }

    return AVERROR(EAGAIN);
}

static void mediacodec_decode_flush(AVCodecContext *avctx)
{
    MediaCodecH264DecContext *s = avctx->priv_data;

    av_packet_unref(&s->buffered_pkt);

    if (ff_mediacodec_dec_flush(avctx, s->ctx) == 1)
        p5_rpu_flush(avctx);
}

static const AVCodecHWConfigInternal *const mediacodec_hw_configs[] = {
    &(const AVCodecHWConfigInternal) {
        .public          = {
            .pix_fmt     = AV_PIX_FMT_MEDIACODEC,
            .methods     = AV_CODEC_HW_CONFIG_METHOD_AD_HOC |
                           AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX,
            .device_type = AV_HWDEVICE_TYPE_MEDIACODEC,
        },
        .hwaccel         = NULL,
    },
    NULL
};

#define OFFSET(x) offsetof(MediaCodecH264DecContext, x)
#define VD AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_DECODING_PARAM
static const AVOption ff_mediacodec_vdec_options[] = {
    { "delay_flush", "Delay flush until hw output buffers are returned to the decoder",
                     OFFSET(delay_flush), AV_OPT_TYPE_BOOL, {.i64 = 0}, 0, 1, VD },
    { "ndk_codec", "Use MediaCodec from NDK",
                   OFFSET(use_ndk_codec), AV_OPT_TYPE_BOOL, {.i64 = -1}, -1, 1, VD },
    { NULL }
};

#define DECLARE_MEDIACODEC_VCLASS(short_name)                   \
static const AVClass ff_##short_name##_mediacodec_dec_class = { \
    .class_name = #short_name "_mediacodec",                    \
    .item_name  = av_default_item_name,                         \
    .option     = ff_mediacodec_vdec_options,                   \
    .version    = LIBAVUTIL_VERSION_INT,                        \
};

#define DECLARE_MEDIACODEC_VDEC(short_name, full_name, codec_id, bsf)                          \
DECLARE_MEDIACODEC_VCLASS(short_name)                                                          \
const FFCodec ff_ ## short_name ## _mediacodec_decoder = {                                     \
    .p.name         = #short_name "_mediacodec",                                               \
    CODEC_LONG_NAME(full_name " Android MediaCodec decoder"),                                  \
    .p.type         = AVMEDIA_TYPE_VIDEO,                                                      \
    .p.id           = codec_id,                                                                \
    .p.priv_class   = &ff_##short_name##_mediacodec_dec_class,                                 \
    .priv_data_size = sizeof(MediaCodecH264DecContext),                                        \
    .init           = mediacodec_decode_init,                                                  \
    FF_CODEC_RECEIVE_FRAME_CB(mediacodec_receive_frame),                                       \
    .flush          = mediacodec_decode_flush,                                                 \
    .close          = mediacodec_decode_close,                                                 \
    .p.capabilities = AV_CODEC_CAP_DELAY | AV_CODEC_CAP_AVOID_PROBING | AV_CODEC_CAP_HARDWARE, \
    .caps_internal  = FF_CODEC_CAP_NOT_INIT_THREADSAFE,                                        \
    .bsfs           = bsf,                                                                     \
    .hw_configs     = mediacodec_hw_configs,                                                   \
    .p.wrapper_name = "mediacodec",                                                            \
};                                                                                             \

#if CONFIG_H264_MEDIACODEC_DECODER
DECLARE_MEDIACODEC_VDEC(h264, "H.264", AV_CODEC_ID_H264, "h264_mp4toannexb")
#endif

#if CONFIG_HEVC_MEDIACODEC_DECODER
DECLARE_MEDIACODEC_VDEC(hevc, "H.265", AV_CODEC_ID_HEVC, "hevc_mp4toannexb")
#endif

#if CONFIG_MPEG2_MEDIACODEC_DECODER
DECLARE_MEDIACODEC_VDEC(mpeg2, "MPEG-2", AV_CODEC_ID_MPEG2VIDEO, NULL)
#endif

#if CONFIG_MPEG4_MEDIACODEC_DECODER
DECLARE_MEDIACODEC_VDEC(mpeg4, "MPEG-4", AV_CODEC_ID_MPEG4, NULL)
#endif

#if CONFIG_VP8_MEDIACODEC_DECODER
DECLARE_MEDIACODEC_VDEC(vp8, "VP8", AV_CODEC_ID_VP8, NULL)
#endif

#if CONFIG_VP9_MEDIACODEC_DECODER
DECLARE_MEDIACODEC_VDEC(vp9, "VP9", AV_CODEC_ID_VP9, NULL)
#endif

#if CONFIG_AV1_MEDIACODEC_DECODER
DECLARE_MEDIACODEC_VDEC(av1, "AV1", AV_CODEC_ID_AV1, NULL)
#endif

#define AD AV_OPT_FLAG_AUDIO_PARAM | AV_OPT_FLAG_DECODING_PARAM
static const AVOption ff_mediacodec_adec_options[] = {
    { "ndk_codec", "Use MediaCodec from NDK",
                   OFFSET(use_ndk_codec), AV_OPT_TYPE_BOOL, {.i64 = -1}, -1, 1, AD },
    { NULL }
};

#define DECLARE_MEDIACODEC_ACLASS(short_name)                   \
static const AVClass ff_##short_name##_mediacodec_dec_class = { \
    .class_name = #short_name "_mediacodec",                    \
    .item_name  = av_default_item_name,                         \
    .option     = ff_mediacodec_adec_options,                   \
    .version    = LIBAVUTIL_VERSION_INT,                        \
};

#define DECLARE_MEDIACODEC_ADEC(short_name, full_name, codec_id, bsf)                          \
DECLARE_MEDIACODEC_VCLASS(short_name)                                                          \
const FFCodec ff_ ## short_name ## _mediacodec_decoder = {                                     \
    .p.name         = #short_name "_mediacodec",                                               \
    CODEC_LONG_NAME(full_name " Android MediaCodec decoder"),                                  \
    .p.type         = AVMEDIA_TYPE_AUDIO,                                                      \
    .p.id           = codec_id,                                                                \
    .p.priv_class   = &ff_##short_name##_mediacodec_dec_class,                                 \
    .priv_data_size = sizeof(MediaCodecH264DecContext),                                        \
    .init           = mediacodec_decode_init,                                                  \
    FF_CODEC_RECEIVE_FRAME_CB(mediacodec_receive_frame),                                       \
    .flush          = mediacodec_decode_flush,                                                 \
    .close          = mediacodec_decode_close,                                                 \
    .p.capabilities = AV_CODEC_CAP_DELAY | AV_CODEC_CAP_HARDWARE,                              \
    .caps_internal  = FF_CODEC_CAP_NOT_INIT_THREADSAFE,                                        \
    .bsfs           = bsf,                                                                     \
    .p.wrapper_name = "mediacodec",                                                            \
};                                                                                             \

#if CONFIG_AAC_MEDIACODEC_DECODER
DECLARE_MEDIACODEC_ADEC(aac, "AAC", AV_CODEC_ID_AAC, "aac_adtstoasc")
#endif

#if CONFIG_AMRNB_MEDIACODEC_DECODER
DECLARE_MEDIACODEC_ADEC(amrnb, "AMR-NB", AV_CODEC_ID_AMR_NB, NULL)
#endif

#if CONFIG_AMRWB_MEDIACODEC_DECODER
DECLARE_MEDIACODEC_ADEC(amrwb, "AMR-WB", AV_CODEC_ID_AMR_WB, NULL)
#endif

#if CONFIG_MP3_MEDIACODEC_DECODER
DECLARE_MEDIACODEC_ADEC(mp3, "MP3", AV_CODEC_ID_MP3, NULL)
#endif
