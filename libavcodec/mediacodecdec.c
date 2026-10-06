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
#include "libavutil/dovi_meta.h"
#include "libavutil/mem.h"
#include "libavutil/opt.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/pixfmt.h"
#include "libavutil/internal.h"

#include "avcodec.h"
#include "codec_internal.h"
#include "decode.h"
#include "h264_parse.h"
#include "h264_ps.h"
#include "hevc/parse.h"
#include "hwconfig.h"
#include "internal.h"
#include "jni.h"
#include "mediacodec_dovi.h"
#include "mediacodec_wrapper.h"
#include "mediacodecdec_common.h"

typedef struct MediaCodecH264DecContext {

    AVClass *avclass;

    MediaCodecDecContext *ctx;

    AVPacket buffered_pkt;

    int delay_flush;
    int amlogic_mpeg2_api23_workaround;

    int use_ndk_codec;

    int dovi;
    int native_dv;
    int native_dv_diag;
    int native_dv_api_version;
    int native_dv_active;
    char *mediacodec_mime;
    char *mediacodec_name;
    FFMediacodecDovi *dovi_export;
} MediaCodecH264DecContext;

/* Runtime facts describe codec configuration, not visible/HDR acceptance. */
static void mediacodec_clear_runtime_facts(MediaCodecH264DecContext *s)
{
    s->native_dv_active = 0;
    av_freep(&s->mediacodec_mime);
    av_freep(&s->mediacodec_name);
}

static int mediacodec_publish_runtime_facts(AVCodecContext *avctx,
                                           const char *mime)
{
    MediaCodecH264DecContext *s = avctx->priv_data;
    char *mime_copy;
    char *name_copy;

    if (!s->ctx || !s->ctx->codec || !s->ctx->codec_name || !mime)
        return AVERROR_EXTERNAL;

    mime_copy = av_strdup(mime);
    /* NDK fallback uses the MIME as codec_name when getName is unavailable.
     * Export an owned empty string for an unknown name, not a fabricated one. */
    name_copy = av_strdup(strcmp(s->ctx->codec_name, mime) ? s->ctx->codec_name : "");
    if (!mime_copy || !name_copy) {
        av_free(mime_copy);
        av_free(name_copy);
        return AVERROR(ENOMEM);
    }

    mediacodec_clear_runtime_facts(s);
    s->mediacodec_mime = mime_copy;
    s->mediacodec_name = name_copy;
    s->native_dv_active = s->native_dv && s->ctx->surface &&
                          !strcmp(mime, "video/dolby-vision");
    return 0;
}

static av_cold int mediacodec_decode_close(AVCodecContext *avctx)
{
    MediaCodecH264DecContext *s = avctx->priv_data;

    mediacodec_clear_runtime_facts(s);
    ff_mediacodec_dovi_free(avctx, &s->dovi_export);

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
static int hevc_set_extradata(AVCodecContext *avctx, FFAMediaFormat *format,
                               MediaCodecNativeDvDiag *diag)
{
    int i;
    int ret;
    MediaCodecH264DecContext *s = avctx->priv_data;

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
                                   &ps, &sei, &is_nalff, &nal_length_size,
                                   s->native_dv ? AV_EF_EXPLODE : 0, 1, avctx);
    if (ret < 0) {
        if (s->native_dv)
            av_log(avctx, AV_LOG_ERROR,
                   "native_dv initialization parameter sets could not be parsed\n");
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

    if (s->native_dv) {
        /* A fresh native DV codec must receive real initialization sets. Use
         * the parsed references, not hvcC array counts or an unrelated VPS. */
        vps = NULL;
        pps = NULL;
        sps = NULL;
        for (i = 0; i < HEVC_MAX_PPS_COUNT; i++) {
            const HEVCPPS *candidate = ps.pps_list[i];
            const HEVCSPS *candidate_sps;
            if (!candidate || candidate->sps_id >= HEVC_MAX_SPS_COUNT)
                continue;
            candidate_sps = ps.sps_list[candidate->sps_id];
            if (!candidate_sps || candidate_sps->vps_id >= HEVC_MAX_VPS_COUNT ||
                !ps.vps_list[candidate_sps->vps_id])
                continue;
            pps = candidate;
            sps = candidate_sps;
            vps = ps.vps_list[sps->vps_id];
            break;
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
        ff_mediacodec_diag_blob(avctx, diag, "csd_submitted", data, data_size);

        av_freep(&data);
    } else {
        const int warn = is_nalff && avctx->codec_tag == MKTAG('h','v','c','1');
        ff_mediacodec_diag_log(avctx, diag,
               "native_dv_diag decoder=%"PRIu64" epoch=%d event=csd_not_submitted reason=params_missing\n",
               diag->decoder_id, diag->epoch);
        av_log(avctx, warn ? AV_LOG_WARNING : AV_LOG_DEBUG,
               "Could not extract VPS/PPS/SPS from extradata\n");
        if (s->native_dv) {
            av_log(avctx, AV_LOG_ERROR,
                   "native_dv requires a parsed PPS/SPS/VPS reference chain before codec start\n");
            ret = AVERROR_INVALIDDATA;
        } else {
            ret = 0;
        }
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

/* Whether the single-layer native-DV route accepts this configuration:
 * BL+RPU and no EL with profile 5 or profile 8 (the RPU stays in the
 * elementary stream for the Dolby Vision decoder). */
static int native_dv_accepts(const AVDOVIDecoderConfigurationRecord *dovi)
{
    return dovi->rpu_present_flag == 1 && dovi->bl_present_flag == 1 &&
           dovi->el_present_flag == 0 &&
           (dovi->dv_profile == 5 || dovi->dv_profile == 8);
}

/* Android CodecProfileLevel key to signal in the MediaFormat. P5 maps to
 * DolbyVisionProfileDvheStn (0x20), which the API 24 LG decoder declares
 * and which is verified on-device. P8 signals DolbyVisionProfileDvheSt in
 * its modern-SDK value (0x100): on the API 24 LG device every P8 signaling
 * is refused (keyless/0x2/0x100 fail configure; 0x20 passes but the decoded
 * buffers never reach the Surface), so P8 keeps the fail-fast 0x100, which
 * is also forward-correct on API 27+. Codec lookup for non-0x20 keys stays
 * MIME-only (see mediacodecdec_common.c) because declared capabilities
 * omit them. */
static int native_dv_android_profile_key(const AVDOVIDecoderConfigurationRecord *dovi)
{
    if (dovi->dv_profile == 5)
        return 0x20;
    if (dovi->dv_profile == 8)
        return 0x100;
    return 0;
}

/* Experimental opt-in route: preserve HEVC CSD, access units and RPUs. */
static int mediacodec_validate_native_dv(AVCodecContext *avctx)
{
    const AVPacketSideData *sd;
    const AVDOVIDecoderConfigurationRecord *dovi;

    if (avctx->codec_id != AV_CODEC_ID_HEVC) {
        av_log(avctx, AV_LOG_ERROR, "native_dv requires an HEVC decoder\n");
        return AVERROR(EINVAL);
    }

    sd = ff_get_coded_side_data(avctx, AV_PKT_DATA_DOVI_CONF);
    if (!sd || !sd->data || sd->size < sizeof(*dovi)) {
        av_log(avctx, AV_LOG_ERROR, "native_dv requires a valid DOVI configuration record\n");
        return AVERROR_INVALIDDATA;
    }
    dovi = (const AVDOVIDecoderConfigurationRecord *)sd->data;
    if (!native_dv_accepts(dovi)) {
        av_log(avctx, AV_LOG_ERROR,
               "native_dv requires single-layer P5 or P8 with BL/RPU and no EL "
               "(profile=%u, BL=%u, RPU=%u, EL=%u)\n",
               dovi->dv_profile, dovi->bl_present_flag,
               dovi->rpu_present_flag, dovi->el_present_flag);
        return AVERROR_INVALIDDATA;
    }

    return 0;
}

static av_cold int mediacodec_decode_init(AVCodecContext *avctx)
{
    int ret;
    int sdk_int;

    const char *codec_mime = NULL;

    FFAMediaFormat *format = NULL;
    MediaCodecNativeDvDiag diag = { 0 };
    MediaCodecH264DecContext *s = avctx->priv_data;

    /* READONLY AVOptions are skipped by av_opt_set_defaults(). The static
     * schema default is queryable before open; initialize its runtime value. */
    s->native_dv_api_version = 1;
    mediacodec_clear_runtime_facts(s);

    ff_mediacodec_diag_begin(avctx, &diag, s->native_dv && s->native_dv_diag);
    if (diag.enabled) {
        const AVPacketSideData *sd = av_packet_side_data_get(avctx->coded_side_data,
                                                            avctx->nb_coded_side_data,
                                                            AV_PKT_DATA_DOVI_CONF);
        const uint8_t *conf = sd ? sd->data : NULL;
        size_t conf_size = sd ? sd->size : 0;
        ff_mediacodec_diag_blob(avctx, &diag, "extradata", avctx->extradata,
                                avctx->extradata_size > 0 ? avctx->extradata_size : 0);
        ff_mediacodec_diag_blob(avctx, &diag, "dovi_conf", conf, conf_size);
        if (diag.enabled && conf_size >= sizeof(AVDOVIDecoderConfigurationRecord)) {
            const AVDOVIDecoderConfigurationRecord *d = (const void *)conf;
            ff_mediacodec_diag_log(avctx, &diag,
                   "native_dv_diag decoder=%"PRIu64" event=dovi_fields profile=%d level=%d bl=%d el=%d rpu=%d compatibility=%d\n",
                   diag.decoder_id, d->dv_profile, d->dv_level, d->bl_present_flag,
                   d->el_present_flag, d->rpu_present_flag, d->dv_bl_signal_compatibility_id);
        }
    }
    if (s->native_dv) {
        ret = mediacodec_validate_native_dv(avctx);
        if (ret < 0)
            goto done;
    }

    ret = ff_mediacodec_dovi_alloc(avctx, &s->dovi_export, s->dovi);
    if (ret < 0)
        goto done;

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
        codec_mime = s->native_dv ? "video/dolby-vision" : "video/hevc";
        if (s->native_dv) {
            const AVPacketSideData *sd_dovi =
                ff_get_coded_side_data(avctx, AV_PKT_DATA_DOVI_CONF);
            /* mediacodec_validate_native_dv() has already accepted the
             * configuration, so the record is present and the key is 0x20
             * (P5) or 0x100 (P8; see native_dv_android_profile_key). */
            const int profile_key =
                sd_dovi && sd_dovi->data
                    ? native_dv_android_profile_key(
                          (const AVDOVIDecoderConfigurationRecord *)sd_dovi->data)
                    : 0;
            if (profile_key)
                ff_AMediaFormat_setInt32(format, "profile", profile_key);
            av_log(avctx, AV_LOG_INFO,
                   "native_dv requested: MIME=%s, Android profile key=0x%x%s; "
                   "preserving HEVC CSD, access units and RPU\n",
                   codec_mime, profile_key,
                   profile_key ? "" : " (omitted)");
        }

        ret = hevc_set_extradata(avctx, format, &diag);
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

    if (diag.requested) {
        s->ctx->native_dv_diag = diag;
        diag.requested = diag.enabled = 0; /* common independently owns the evidence */
    }
    s->ctx->delay_flush = s->delay_flush;
    s->ctx->use_ndk_codec = s->use_ndk_codec;

    if ((ret = ff_mediacodec_dec_init(avctx, s->ctx, codec_mime, format)) < 0) {
        s->ctx = NULL;
        goto done;
    }

    if (avctx->codec_type == AVMEDIA_TYPE_VIDEO) {
        ret = mediacodec_publish_runtime_facts(avctx, codec_mime);
        if (ret < 0)
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
        if (diag.requested) {
            av_log(avctx, AV_LOG_INFO,
                   "native_dv_diag decoder=%"PRIu64" event=init_failed status=%d\n",
                   diag.decoder_id, ret);
            ff_mediacodec_diag_close(avctx, &diag);
        }
        mediacodec_decode_close(avctx);
    }

    return ret;
}

/* Receive one frame from MediaCodec and attach the RPU side data tracked for
 * its presentationTimeUs. */
static int mediacodec_receive_frame_internal(AVCodecContext *avctx,
                                             AVFrame *frame, bool wait)
{
    MediaCodecH264DecContext *s = avctx->priv_data;
    int64_t pts_us = AV_NOPTS_VALUE;
    int ret = ff_mediacodec_dec_receive(avctx, s->ctx, frame, wait, &pts_us);

    if (!ret)
        ff_mediacodec_dovi_attach_output(avctx, s->dovi_export, frame, pts_us);
    return ret;
}

static int mediacodec_receive_frame_impl(AVCodecContext *avctx, AVFrame *frame)
{
    MediaCodecH264DecContext *s = avctx->priv_data;
    int ret;
    ssize_t index;

    /* In delay_flush mode, wait until the user has released or rendered
       all retained frames. */
    if (s->delay_flush && ff_mediacodec_dec_is_flushing(avctx, s->ctx)) {
        int flush_ret = ff_mediacodec_dec_flush(avctx, s->ctx);
        if (flush_ret < 0)
            return flush_ret;
        if (!flush_ret) {
            return AVERROR(EAGAIN);
        }
        if (flush_ret == 1)
            ff_mediacodec_dovi_flush(avctx, s->dovi_export);
    }

    /* poll for new frame */
    ret = mediacodec_receive_frame_internal(avctx, frame, false);
    if (ret != AVERROR(EAGAIN))
        return ret;

    /* feed decoder */
    while (1) {
        if (s->ctx->current_input_buffer < 0 && !s->ctx->draining) {
            /* poll for input space */
            index = ff_AMediaCodec_dequeueInputBuffer(s->ctx->codec, 0);
            if (index < 0) {
                /* no space, block for an output frame to appear */
                ret = mediacodec_receive_frame_internal(avctx, frame, true);
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
            return mediacodec_receive_frame_internal(avctx, frame, true);
        } else if (ret == AVERROR(EAGAIN) && s->ctx->current_input_buffer < 0) {
            return mediacodec_receive_frame_internal(avctx, frame, true);
        } else if (ret < 0) {
            return ret;
        }
        ff_mediacodec_diag_au(avctx, &s->ctx->native_dv_diag, &s->buffered_pkt);
        /* Register the RPU before queueing: a split AU can produce output
         * early. */
        ff_mediacodec_dovi_track_input(avctx, s->dovi_export, &s->buffered_pkt,
                                       ff_mediacodec_pts_to_us(avctx,
                                                               s->buffered_pkt.pts));
    }

    return AVERROR(EAGAIN);
}

static int mediacodec_receive_frame(AVCodecContext *avctx, AVFrame *frame)
{
    MediaCodecH264DecContext *s = avctx->priv_data;
    int ret = mediacodec_receive_frame_impl(avctx, frame);

    if (ret < 0 && ret != AVERROR(EAGAIN) && ret != AVERROR_EOF)
        mediacodec_clear_runtime_facts(s);
    return ret;
}

static void mediacodec_decode_flush(AVCodecContext *avctx)
{
    MediaCodecH264DecContext *s = avctx->priv_data;
    int ret;

    av_packet_unref(&s->buffered_pkt);

    ret = ff_mediacodec_dec_flush(avctx, s->ctx);
    if (ret == 1)
        ff_mediacodec_dovi_flush(avctx, s->dovi_export);
    else if (ret < 0)
        mediacodec_clear_runtime_facts(s);
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
#define VDX (VD | AV_OPT_FLAG_EXPORT | AV_OPT_FLAG_READONLY)
static const AVOption ff_mediacodec_vdec_options[] = {
    { "native_dv_api_version", "Native Dolby Vision runtime-fact schema version (static default queryable before codec open)",
                              OFFSET(native_dv_api_version), AV_OPT_TYPE_INT, {.i64 = 1}, 1, 1, VDX },
    { "mediacodec_mime", "Configured MediaCodec MIME after successful configure/start",
                         OFFSET(mediacodec_mime), AV_OPT_TYPE_STRING, {.str = NULL}, 0, 0, VDX },
    { "mediacodec_name", "Selected MediaCodec name after successful configure/start (empty if unknown)",
                         OFFSET(mediacodec_name), AV_OPT_TYPE_STRING, {.str = NULL}, 0, 0, VDX },
    { "native_dv_active", "Native P5/P8 Surface codec configured and started; does not prove visible frames or HDR display",
                          OFFSET(native_dv_active), AV_OPT_TYPE_INT, {.i64 = 0}, 0, 1, VDX },
    { "native_dv_diag", "Bounded native DV input evidence (requires native_dv=1)",
                        OFFSET(native_dv_diag), AV_OPT_TYPE_BOOL, {.i64 = 0}, 0, 1, VD },
    { "native_dv", "Experimentally decode single-layer Dolby Vision P5/P8 to a Surface (HEVC only)",
                   OFFSET(native_dv), AV_OPT_TYPE_BOOL, {.i64 = 0}, 0, 1, VD },
    { "delay_flush", "Delay flush until hw output buffers are returned to the decoder",
                     OFFSET(delay_flush), AV_OPT_TYPE_BOOL, {.i64 = 0}, 0, 1, VD },
    { "ndk_codec", "Use MediaCodec from NDK",
                   OFFSET(use_ndk_codec), AV_OPT_TYPE_BOOL, {.i64 = -1}, -1, 1, VD },
    { "dovi", "Export Dolby Vision RPU frame side data (HEVC only)",
              OFFSET(dovi), AV_OPT_TYPE_INT,
              { .i64 = FF_MEDIACODEC_DOVI_AUTO }, 0, FF_MEDIACODEC_DOVI_OFF, VD, .unit = "dovi" },
    { "auto", "Enable for profile 5 streams",
              0, AV_OPT_TYPE_CONST, { .i64 = FF_MEDIACODEC_DOVI_AUTO }, 0, 0, VD, .unit = "dovi" },
    { "on",   "Always enable",
              0, AV_OPT_TYPE_CONST, { .i64 = FF_MEDIACODEC_DOVI_ON },   0, 0, VD, .unit = "dovi" },
    { "off",  "Disable",
              0, AV_OPT_TYPE_CONST, { .i64 = FF_MEDIACODEC_DOVI_OFF },  0, 0, VD, .unit = "dovi" },
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
