/*
 * AVS3-P3 / Audio Vivid decoder using OpenHarmony AudioCodec
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include "config_components.h"

#include <errno.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <time.h>

#include <multimedia/player_framework/native_avcodec_audiocodec.h>

#include "libavutil/channel_layout.h"
#include "libavutil/fifo.h"
#include "libavutil/mem.h"
#include "libavutil/opt.h"
#include "libavutil/samplefmt.h"
#include "libavutil/thread.h"

#include "avcodec.h"
#include "codec_internal.h"
#include "decode.h"
#include "ohcodec.h"
#include "pthread_internal.h"

typedef struct OHAudioQueueItem {
    uint32_t index;
    OH_AVBuffer *buffer;
} OHAudioQueueItem;

typedef struct OHAudioDecContext {
    AVClass *avclass;
    OH_AVCodec *decoder;

    AVMutex input_mutex;
    AVCond input_cond;
    AVMutex output_mutex;
    AVCond output_cond;
    unsigned mutex_cond_cnt;

    AVFifo *input_queue;
    AVFifo *output_queue;
    AVPacket packet;

    int decode_status;
    bool eof_sent;
    atomic_bool accepting_callbacks;
} OHAudioDecContext;

#define OFFSET(x) offsetof(OHAudioDecContext, x)
DEFINE_OFFSET_ARRAY(OHAudioDecContext, mutex_cond, mutex_cond_cnt,
                    (OFFSET(input_mutex), OFFSET(output_mutex)),
                    (OFFSET(input_cond), OFFSET(output_cond)));

static int oh_audio_cond_wait(AVCond *cond, AVMutex *mutex)
{
    struct timespec deadline;

    if (clock_gettime(CLOCK_REALTIME, &deadline) < 0)
        return errno;
    deadline.tv_nsec += 100 * 1000 * 1000;
    if (deadline.tv_nsec >= 1000 * 1000 * 1000) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000 * 1000 * 1000;
    }
    return ff_cond_timedwait(cond, mutex, &deadline);
}

static void oh_audio_on_error(OH_AVCodec *codec, int32_t error,
                              void *userdata)
{
    AVCodecContext *avctx = userdata;
    OHAudioDecContext *s = avctx->priv_data;

    av_log(avctx, AV_LOG_ERROR, "OpenHarmony AudioCodec error: %d\n", error);

    ff_mutex_lock(&s->input_mutex);
    ff_mutex_lock(&s->output_mutex);
    s->decode_status = AVERROR_EXTERNAL;
    ff_cond_broadcast(&s->input_cond);
    ff_cond_broadcast(&s->output_cond);
    ff_mutex_unlock(&s->output_mutex);
    ff_mutex_unlock(&s->input_mutex);
}

static void oh_audio_on_stream_changed(OH_AVCodec *codec, OH_AVFormat *format,
                                       void *userdata)
{
    AVCodecContext *avctx = userdata;
    int32_t value;

    if (OH_AVFormat_GetIntValue(format, OH_MD_KEY_AUD_SAMPLE_RATE, &value) &&
        value > 0)
        avctx->sample_rate = value;

    if (OH_AVFormat_GetIntValue(format, OH_MD_KEY_AUD_CHANNEL_COUNT, &value) &&
        value > 0 && value != avctx->ch_layout.nb_channels) {
        av_channel_layout_uninit(&avctx->ch_layout);
        av_channel_layout_default(&avctx->ch_layout, value);
    }
}

static void oh_audio_on_input(OH_AVCodec *codec, uint32_t index,
                              OH_AVBuffer *buffer, void *userdata)
{
    AVCodecContext *avctx = userdata;
    OHAudioDecContext *s = avctx->priv_data;
    OHAudioQueueItem item = { index, buffer };

    if (!atomic_load_explicit(&s->accepting_callbacks,
                              memory_order_acquire))
        return;

    ff_mutex_lock(&s->input_mutex);
    if (av_fifo_write(s->input_queue, &item, 1) < 0)
        s->decode_status = AVERROR(ENOMEM);
    ff_cond_signal(&s->input_cond);
    ff_mutex_unlock(&s->input_mutex);
}

static void oh_audio_on_output(OH_AVCodec *codec, uint32_t index,
                               OH_AVBuffer *buffer, void *userdata)
{
    AVCodecContext *avctx = userdata;
    OHAudioDecContext *s = avctx->priv_data;
    OHAudioQueueItem item = { index, buffer };

    if (!atomic_load_explicit(&s->accepting_callbacks,
                              memory_order_acquire))
        return;

    ff_mutex_lock(&s->output_mutex);
    if (av_fifo_write(s->output_queue, &item, 1) < 0)
        s->decode_status = AVERROR(ENOMEM);
    ff_cond_signal(&s->output_cond);
    ff_mutex_unlock(&s->output_mutex);
}

static int oh_audio_configure(AVCodecContext *avctx)
{
    OHAudioDecContext *s = avctx->priv_data;
    OH_AVFormat *format;
    OH_AVErrCode err;
    int channels = avctx->ch_layout.nb_channels;
    int sample_rate = avctx->sample_rate;
    uint64_t channel_mask;

    if (channels <= 0) {
        channels = 2;
        av_channel_layout_default(&avctx->ch_layout, channels);
    }
    if (sample_rate <= 0)
        sample_rate = avctx->sample_rate = 48000;

    format = OH_AVFormat_Create();
    if (!format)
        return AVERROR(ENOMEM);

    if (!OH_AVFormat_SetIntValue(format, OH_MD_KEY_AUDIO_SAMPLE_FORMAT,
                                 SAMPLE_S16LE) ||
        !OH_AVFormat_SetIntValue(format, OH_MD_KEY_AUD_CHANNEL_COUNT,
                                 channels) ||
        !OH_AVFormat_SetIntValue(format, OH_MD_KEY_AUD_SAMPLE_RATE,
                                 sample_rate)) {
        OH_AVFormat_Destroy(format);
        return AVERROR_EXTERNAL;
    }

    if (avctx->bit_rate > 0)
        OH_AVFormat_SetLongValue(format, OH_MD_KEY_BITRATE, avctx->bit_rate);

    channel_mask = av_channel_layout_subset(&avctx->ch_layout, UINT64_MAX);
    if (channel_mask)
        OH_AVFormat_SetLongValue(format, OH_MD_KEY_CHANNEL_LAYOUT,
                                 channel_mask);

    if (avctx->extradata && avctx->extradata_size > 0)
        OH_AVFormat_SetBuffer(format, OH_MD_KEY_CODEC_CONFIG,
                              avctx->extradata, avctx->extradata_size);

    err = OH_AudioCodec_Configure(s->decoder, format);
    OH_AVFormat_Destroy(format);
    if (err != AV_ERR_OK) {
        av_log(avctx, AV_LOG_ERROR,
               "Failed to configure Audio Vivid decoder: %d\n", err);
        return ff_oh_err_to_ff_err(err);
    }

    avctx->sample_fmt = AV_SAMPLE_FMT_S16;
    return 0;
}

static av_cold int oh_audio_decode_init(AVCodecContext *avctx)
{
    OHAudioDecContext *s = avctx->priv_data;
    OH_AVCodecCallback callbacks = {
        .onError = oh_audio_on_error,
        .onStreamChanged = oh_audio_on_stream_changed,
        .onNeedInputBuffer = oh_audio_on_input,
        .onNewOutputBuffer = oh_audio_on_output,
    };
    OH_AVErrCode err;
    int ret;

    atomic_init(&s->accepting_callbacks, false);
    ret = ff_pthread_init(s, mutex_cond_offsets);
    if (ret < 0)
        return ret;

    s->input_queue = av_fifo_alloc2(16, sizeof(OHAudioQueueItem),
                                    AV_FIFO_FLAG_AUTO_GROW);
    s->output_queue = av_fifo_alloc2(16, sizeof(OHAudioQueueItem),
                                     AV_FIFO_FLAG_AUTO_GROW);
    if (!s->input_queue || !s->output_queue)
        return AVERROR(ENOMEM);

    s->decoder = OH_AudioCodec_CreateByMime(OH_AVCODEC_MIMETYPE_AUDIO_VIVID,
                                             false);
    if (!s->decoder) {
        av_log(avctx, AV_LOG_ERROR,
               "Audio Vivid decoder is unavailable on this device\n");
        return AVERROR_DECODER_NOT_FOUND;
    }

    ret = oh_audio_configure(avctx);
    if (ret < 0)
        return ret;

    err = OH_AudioCodec_RegisterCallback(s->decoder, callbacks, avctx);
    if (err != AV_ERR_OK)
        return ff_oh_err_to_ff_err(err);

    err = OH_AudioCodec_Prepare(s->decoder);
    if (err != AV_ERR_OK)
        return ff_oh_err_to_ff_err(err);

    atomic_store_explicit(&s->accepting_callbacks, true,
                          memory_order_release);
    err = OH_AudioCodec_Start(s->decoder);
    if (err != AV_ERR_OK) {
        atomic_store_explicit(&s->accepting_callbacks, false,
                              memory_order_release);
        return ff_oh_err_to_ff_err(err);
    }

    return 0;
}

static av_cold int oh_audio_decode_close(AVCodecContext *avctx)
{
    OHAudioDecContext *s = avctx->priv_data;

    atomic_store_explicit(&s->accepting_callbacks, false,
                          memory_order_release);
    if (s->decoder) {
        OH_AudioCodec_Stop(s->decoder);
        OH_AudioCodec_Destroy(s->decoder);
        s->decoder = NULL;
    }

    av_packet_unref(&s->packet);
    av_fifo_freep2(&s->input_queue);
    av_fifo_freep2(&s->output_queue);
    ff_pthread_free(s, mutex_cond_offsets);
    return 0;
}

static int oh_audio_send_packet(AVCodecContext *avctx,
                                const OHAudioQueueItem *input)
{
    OHAudioDecContext *s = avctx->priv_data;
    OH_AVCodecBufferAttr attr = { 0 };
    OH_AVErrCode err;
    uint8_t *dst;
    int capacity;
    int size;

    if (!s->packet.size && !s->eof_sent) {
        attr.flags = AVCODEC_BUFFER_FLAGS_EOS;
        err = OH_AVBuffer_SetBufferAttr(input->buffer, &attr);
        if (err == AV_ERR_OK)
            err = OH_AudioCodec_PushInputBuffer(s->decoder, input->index);
        if (err != AV_ERR_OK)
            return ff_oh_err_to_ff_err(err);
        s->eof_sent = true;
        return 0;
    }

    dst = OH_AVBuffer_GetAddr(input->buffer);
    capacity = OH_AVBuffer_GetCapacity(input->buffer);
    if (!dst || capacity <= 0)
        return AVERROR_EXTERNAL;

    size = FFMIN(s->packet.size, capacity);
    memcpy(dst, s->packet.data, size);

    attr.size = size;
    attr.pts = s->packet.pts == AV_NOPTS_VALUE ? 0 :
               av_rescale_q(s->packet.pts, avctx->pkt_timebase,
                            AV_TIME_BASE_Q);
    if (s->packet.flags & AV_PKT_FLAG_KEY)
        attr.flags = AVCODEC_BUFFER_FLAGS_SYNC_FRAME;

    err = OH_AVBuffer_SetBufferAttr(input->buffer, &attr);
    if (err == AV_ERR_OK)
        err = OH_AudioCodec_PushInputBuffer(s->decoder, input->index);
    if (err != AV_ERR_OK)
        return ff_oh_err_to_ff_err(err);

    if (size < s->packet.size) {
        s->packet.data += size;
        s->packet.size -= size;
    } else {
        av_packet_unref(&s->packet);
    }

    return 0;
}

static int oh_audio_output_frame(AVCodecContext *avctx, AVFrame *frame,
                                 const OHAudioQueueItem *output)
{
    OHAudioDecContext *s = avctx->priv_data;
    OH_AVCodecBufferAttr attr;
    OH_AVErrCode err;
    uint8_t *src;
    int bytes_per_sample;
    int ret;

    err = OH_AVBuffer_GetBufferAttr(output->buffer, &attr);
    if (err != AV_ERR_OK)
        return ff_oh_err_to_ff_err(err);

    if (attr.flags & AVCODEC_BUFFER_FLAGS_EOS) {
        OH_AudioCodec_FreeOutputBuffer(s->decoder, output->index);
        return AVERROR_EOF;
    }

    bytes_per_sample = av_get_bytes_per_sample(AV_SAMPLE_FMT_S16);
    if (attr.size <= 0 || avctx->ch_layout.nb_channels <= 0 ||
        attr.size % (bytes_per_sample * avctx->ch_layout.nb_channels)) {
        OH_AudioCodec_FreeOutputBuffer(s->decoder, output->index);
        return AVERROR(EAGAIN);
    }

    frame->format = AV_SAMPLE_FMT_S16;
    frame->sample_rate = avctx->sample_rate;
    ret = av_channel_layout_copy(&frame->ch_layout, &avctx->ch_layout);
    if (ret < 0)
        goto release;

    frame->nb_samples = attr.size /
        (bytes_per_sample * frame->ch_layout.nb_channels);
    if (frame->nb_samples <= 0) {
        ret = AVERROR_INVALIDDATA;
        goto release;
    }

    ret = ff_get_buffer(avctx, frame, 0);
    if (ret < 0)
        goto release;

    src = OH_AVBuffer_GetAddr(output->buffer);
    if (!src) {
        ret = AVERROR_EXTERNAL;
        goto release;
    }
    memcpy(frame->data[0], src + attr.offset, attr.size);

    frame->pts = av_rescale_q(attr.pts, AV_TIME_BASE_Q,
                              avctx->pkt_timebase);
    ret = 0;

release:
    err = OH_AudioCodec_FreeOutputBuffer(s->decoder, output->index);
    if (ret >= 0 && err != AV_ERR_OK)
        ret = ff_oh_err_to_ff_err(err);
    return ret;
}

static int oh_audio_receive_frame(AVCodecContext *avctx, AVFrame *frame)
{
    OHAudioDecContext *s = avctx->priv_data;

    while (1) {
        OHAudioQueueItem item = { 0 };
        int ret;

        ff_mutex_lock(&s->output_mutex);
        while (!s->decode_status) {
            if (av_fifo_read(s->output_queue, &item, 1) >= 0)
                break;
            if (!s->eof_sent ||
                oh_audio_cond_wait(&s->output_cond, &s->output_mutex) ==
                ETIMEDOUT)
                break;
        }
        ret = s->decode_status;
        ff_mutex_unlock(&s->output_mutex);

        if (item.buffer)
            return oh_audio_output_frame(avctx, frame, &item);
        if (ret < 0)
            return ret;

        if (!s->packet.size) {
            ret = ff_decode_get_packet(avctx, &s->packet);
            if (ret < 0 && ret != AVERROR_EOF)
                return ret;
        }

        ff_mutex_lock(&s->input_mutex);
        while (!s->decode_status) {
            if (av_fifo_read(s->input_queue, &item, 1) >= 0)
                break;
            if (oh_audio_cond_wait(&s->input_cond, &s->input_mutex) ==
                ETIMEDOUT)
                break;
        }
        ret = s->decode_status;
        ff_mutex_unlock(&s->input_mutex);

        if (ret < 0)
            return ret;
        if (!item.buffer)
            return AVERROR(EAGAIN);

        ret = oh_audio_send_packet(avctx, &item);
        if (ret < 0)
            return ret;
    }
}

static void oh_audio_decode_flush(AVCodecContext *avctx)
{
    OHAudioDecContext *s = avctx->priv_data;

    atomic_store_explicit(&s->accepting_callbacks, false,
                          memory_order_release);
    OH_AudioCodec_Flush(s->decoder);

    ff_mutex_lock(&s->input_mutex);
    ff_mutex_lock(&s->output_mutex);
    av_fifo_reset2(s->input_queue);
    av_fifo_reset2(s->output_queue);
    s->decode_status = 0;
    s->eof_sent = false;
    av_packet_unref(&s->packet);
    ff_mutex_unlock(&s->output_mutex);
    ff_mutex_unlock(&s->input_mutex);

    atomic_store_explicit(&s->accepting_callbacks, true,
                          memory_order_release);
    if (OH_AudioCodec_Start(s->decoder) != AV_ERR_OK)
        atomic_store_explicit(&s->accepting_callbacks, false,
                              memory_order_release);
}

const FFCodec ff_av3a_oh_decoder = {
    .p.name         = "av3a_ohcodec",
    CODEC_LONG_NAME("AVS3-P3 / Audio Vivid OpenHarmony AudioCodec"),
    .p.type         = AVMEDIA_TYPE_AUDIO,
    .p.id           = AV_CODEC_ID_AVS3DA,
    .priv_data_size = sizeof(OHAudioDecContext),
    .init           = oh_audio_decode_init,
    FF_CODEC_RECEIVE_FRAME_CB(oh_audio_receive_frame),
    .flush          = oh_audio_decode_flush,
    .close          = oh_audio_decode_close,
    .p.capabilities = AV_CODEC_CAP_DELAY | AV_CODEC_CAP_AVOID_PROBING |
                      AV_CODEC_CAP_HARDWARE,
    .caps_internal  = FF_CODEC_CAP_INIT_CLEANUP,
    .p.wrapper_name = "ohcodec",
};
