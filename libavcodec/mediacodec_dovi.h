/*
 * Dolby Vision RPU side-data export for Android MediaCodec decoders
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

#ifndef AVCODEC_MEDIACODEC_DOVI_H
#define AVCODEC_MEDIACODEC_DOVI_H

#include "avcodec.h"

/**
 * Dolby Vision RPU export mode, settable through the "dovi" AVOption.
 */
#define FF_MEDIACODEC_DOVI_AUTO 0  /**< Enable for profile 5 streams. */
#define FF_MEDIACODEC_DOVI_ON   1  /**< Always enable (any profile). */
#define FF_MEDIACODEC_DOVI_OFF  2  /**< Disable. */

typedef struct FFMediacodecDovi FFMediacodecDovi;

/**
 * Set up RPU tracking if requested by mode and applicable to the stream.
 * *pdovi is set to NULL when tracking is disabled.
 */
int ff_mediacodec_dovi_alloc(AVCodecContext *avctx, FFMediacodecDovi **pdovi,
                             int mode);

/**
 * Free the tracker after logging a lifetime summary. Accepts NULL.
 */
void ff_mediacodec_dovi_free(AVCodecContext *avctx, FFMediacodecDovi **pdovi);

/**
 * Parse the RPU of an access unit (in decode order) and register it under
 * pts_us, the presentationTimeUs MediaCodec will queue for it.
 */
void ff_mediacodec_dovi_track_input(AVCodecContext *avctx,
                                    FFMediacodecDovi *dovi,
                                    const AVPacket *pkt, int64_t pts_us);

/**
 * Attach AV_FRAME_DATA_DOVI_METADATA and AV_FRAME_DATA_DOVI_RPU_BUFFER to an
 * output frame whose MediaCodec presentationTimeUs is pts_us.
 */
void ff_mediacodec_dovi_attach_output(AVCodecContext *avctx,
                                      FFMediacodecDovi *dovi,
                                      AVFrame *frame, int64_t pts_us);

/**
 * Drop all tracked state on a decoder flush.
 */
void ff_mediacodec_dovi_flush(AVCodecContext *avctx, FFMediacodecDovi *dovi);

#endif /* AVCODEC_MEDIACODEC_DOVI_H */
