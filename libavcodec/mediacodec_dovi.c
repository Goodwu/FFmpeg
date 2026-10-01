/*
 * Dolby Vision RPU side-data export for Android MediaCodec decoders
 *
 * MediaCodec carries no per-buffer user data other than presentationTimeUs,
 * so the Dolby Vision RPU NALs (type 62) fed to a hardware decoder are lost
 * on its output side. This module parses RPUs on the input path -- in decode
 * order, like the software HEVC decoder -- snapshots the parsed metadata per
 * access unit, and re-attaches it to output frames by matching the
 * presentationTimeUs queued on input against the one reported on output.
 *
 * The side data produced here is identical to what hevcdec attaches, so
 * downstream consumers need not distinguish software from hardware decode.
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

#include "libavutil/dovi_meta.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/mem.h"

#include "avcodec.h"
#include "decode.h"
#include "dovi_rpu.h"
#include "h2645_parse.h"

#include "mediacodec_dovi.h"

/*
 * Ring of access units between input registration and output attach, keyed
 * by the presentationTimeUs queued on input. In-flight access units are
 * bounded by MediaCodec's input and output buffer counts (a few dozen at
 * most), so 128 leaves a generous margin even for codecs holding several
 * dropped frames.
 */
#define P5_RPU_MAX_ENTRIES 128

typedef struct P5RpuEntry {
    unsigned seq;
    unsigned epoch;
    int64_t pts_us;
    uint32_t rpu_hash;
    unsigned rpu_size;
    unsigned rpu_count;
    int valid;
    int consumed;
    int duplicate_pts;
    int missing_pts;
    int parse_error;
    AVDOVIMetadata *metadata;
    int metadata_size;
    uint8_t *raw_rpu;
    int raw_rpu_size;
} P5RpuEntry;

typedef enum P5RpuStatus {
    P5_RPU_MATCH,
    P5_RPU_MISSING_INPUT_PTS,
    P5_RPU_MISSING_OUTPUT_PTS,
    P5_RPU_UNMATCHED_OUTPUT_PTS,
    P5_RPU_DUPLICATE_INPUT_PTS,
    P5_RPU_DUPLICATE_OUTPUT_PTS,
    P5_RPU_PARSE_ERROR,
    P5_RPU_MULTIPLE_RPU,
    P5_RPU_MISSING_RPU,
    P5_RPU_MISSING_PARSED_METADATA,
    P5_RPU_EXISTING_DOVI_SIDE_DATA,
    P5_RPU_ATTACH_ERROR,
} P5RpuStatus;

static const char *const p5_rpu_status_names[] = {
    "match",
    "missing input pts",
    "missing output pts",
    "unmatched output pts",
    "duplicate input pts",
    "duplicate output pts",
    "parse error",
    "multiple rpu",
    "missing rpu",
    "missing parsed metadata",
    "existing dovi side data",
    "attach error",
};

struct FFMediacodecDovi {
    DOVIContext dovi;
    /* Persistent unescape buffer; sized for the RPU NAL only. */
    H2645RBSP rbsp;
    H2645NAL nal;
    P5RpuEntry *entries;
    unsigned epoch;
    unsigned inputs;
    unsigned outputs;
    unsigned matched;
    unsigned discarded;
    unsigned input_errors;
    unsigned output_errors;
};

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

/*
 * Single linear scan over the access unit: count NAL 62 units, validate their
 * NAL headers and remember the last one. No data is copied here; hevcdec also
 * only considers the last RPU NAL of an access unit.
 */
static void p5_rpu_scan_nal(P5RpuEntry *entry, const uint8_t **rpu_nal,
                            int *rpu_nal_size, const uint8_t *data, int size)
{
    if (((data[0] >> 1) & 0x3f) != 62)
        return;

    entry->rpu_count++;
    /* A Dolby Vision RPU carries nuh_layer_id 0 and temporal_id_plus1 1. */
    if (size < 3 || ((data[0] & 1) << 5 | data[1] >> 3) != 0 ||
        (data[1] & 7) != 1) {
        entry->parse_error = 1;
        return;
    }
    *rpu_nal     = data;
    *rpu_nal_size = size;

    entry->rpu_size = size;
    entry->rpu_hash = 2166136261U;
    for (int i = 2; i < size; i++)
        entry->rpu_hash = (entry->rpu_hash ^ data[i]) * 16777619U;
}

/* ff_h2645_extract_rbsp() writes at offset rbsp_buffer_size (reset to zero
 * below) and appends AV_INPUT_BUFFER_PADDING_SIZE zero bytes. */
static int p5_rpu_rbsp_reserve(struct FFMediacodecDovi *d, int size)
{
    int needed = size + AV_INPUT_BUFFER_PADDING_SIZE;
    uint8_t *buf;

    if (d->rbsp.rbsp_buffer_alloc_size < needed) {
        int sz = needed + needed / 16 + 32;

        buf = av_realloc(d->rbsp.rbsp_buffer, sz);
        if (!buf)
            return AVERROR(ENOMEM);
        d->rbsp.rbsp_buffer = buf;
        d->rbsp.rbsp_buffer_alloc_size = sz;
    }
    d->rbsp.rbsp_buffer_size = 0;
    return 0;
}

/*
 * Parse the access unit and fill a fresh entry. Called once per packet, right
 * before the packet is handed to the decoder, so the entry is complete by the
 * time MediaCodec can produce output for it.
 */
static void p5_rpu_parse_packet(AVCodecContext *avctx, struct FFMediacodecDovi *d,
                                P5RpuEntry *entry, const AVPacket *pkt,
                                int64_t pts_us)
{
    const uint8_t *sd;
    const uint8_t *rpu_nal = NULL;
    int rpu_nal_size = 0;
    int header, start, ret;
    size_t sd_size;

    /* A DOVI configuration record from the input packet updates the active
     * configuration, like hevcdec does. */
    sd = av_packet_get_side_data(pkt, AV_PKT_DATA_DOVI_CONF, &sd_size);
    if (sd && sd_size >= sizeof(d->dovi.cfg)) {
        int old_profile = d->dovi.cfg.dv_profile;

        memcpy(&d->dovi.cfg, sd, sizeof(d->dovi.cfg));
        if (old_profile && old_profile != d->dovi.cfg.dv_profile)
            av_log(avctx, AV_LOG_DEBUG,
                   "New DOVI configuration record from input packet "
                   "(profile %d -> %u).\n",
                   old_profile, d->dovi.cfg.dv_profile);
    }

    entry->epoch      = d->epoch;
    entry->missing_pts = pkt->pts == AV_NOPTS_VALUE;
    entry->pts_us     = pts_us;

    if (!pkt->data || pkt->size <= 0) {
        entry->parse_error = 1;
        return;
    }

    start = p5_rpu_start_code(pkt->data, pkt->size, 0, &header);
    if (start < 0 || start > 3) {
        /* The wrapper's bitstream filter guarantees Annex B. */
        entry->parse_error = 1;
        return;
    }
    while (start >= 0) {
        int next_header, next = p5_rpu_start_code(pkt->data, pkt->size,
                                                  header, &next_header);
        p5_rpu_scan_nal(entry, &rpu_nal, &rpu_nal_size, pkt->data + header,
                        (next < 0 ? pkt->size : next) - header);
        start = next;
        if (next >= 0)
            header = next_header;
    }

    if (entry->rpu_count != 1 || entry->parse_error)
        return;

    ret = p5_rpu_rbsp_reserve(d, rpu_nal_size);
    if (ret < 0) {
        entry->parse_error = 1;
        return;
    }
    ret = ff_h2645_extract_rbsp(rpu_nal, rpu_nal_size, &d->rbsp, &d->nal, 1);
    if (ret < 0 || d->nal.size <= 2 || d->nal.raw_size <= 2) {
        entry->parse_error = 1;
        return;
    }
    ret = ff_dovi_rpu_parse(&d->dovi, d->nal.data + 2, d->nal.size - 2,
                            avctx->err_recognition);
    if (ret < 0) {
        entry->parse_error = 1;
        return;
    }
    entry->metadata_size = ff_dovi_get_metadata(&d->dovi, &entry->metadata);
    if (entry->metadata_size <= 0) {
        entry->metadata = NULL;
        entry->parse_error = 1;
        return;
    }
    /* AV_FRAME_DATA_DOVI_RPU_BUFFER carries the RPU with emulation
     * prevention bytes, like hevcdec. */
    entry->raw_rpu_size = d->nal.raw_size - 2;
    entry->raw_rpu = av_memdup(d->nal.raw_data + 2, entry->raw_rpu_size);
    if (!entry->raw_rpu)
        entry->parse_error = 1;
}

static void p5_rpu_commit(AVCodecContext *avctx, struct FFMediacodecDovi *d,
                          P5RpuEntry *entry)
{
    P5RpuStatus status = P5_RPU_MATCH;
    unsigned seq  = d->inputs++;
    unsigned slot_idx = seq % P5_RPU_MAX_ENTRIES;
    P5RpuEntry *slot = &d->entries[slot_idx];
    if (slot->valid && !slot->consumed) {
        d->discarded++;
        av_log(avctx, AV_LOG_DEBUG,
               "Dolby Vision RPU entry evicted before its frame was output "
               "(input seq %u, %u discarded).\n",
               slot->seq, d->discarded);
    }

    /* Only unconsumed entries can compete for an output frame, and the entry
     * about to be replaced is gone by the time this one reaches the output. */
    if (!entry->missing_pts) {
        for (unsigned j = 0; j < P5_RPU_MAX_ENTRIES; j++) {
            P5RpuEntry *old = &d->entries[j];

            if (j == slot_idx)
                continue;
            if (old->valid && !old->consumed && old->epoch == d->epoch &&
                !old->missing_pts && old->pts_us == entry->pts_us)
                entry->duplicate_pts = 1;
        }
    }

    p5_rpu_clear_entry(slot);
    *slot = *entry;
    slot->seq   = seq;
    slot->valid = 1;

    if (entry->missing_pts)
        status = P5_RPU_MISSING_INPUT_PTS;
    else if (entry->parse_error)
        status = P5_RPU_PARSE_ERROR;
    else if (entry->rpu_count != 1)
        status = entry->rpu_count ? P5_RPU_MULTIPLE_RPU : P5_RPU_MISSING_RPU;
    else if (entry->duplicate_pts)
        status = P5_RPU_DUPLICATE_INPUT_PTS;

    if (status != P5_RPU_MATCH) {
        d->input_errors++;
        if (d->input_errors <= 20 || d->input_errors % 1000 == 0)
            av_log(avctx, AV_LOG_WARNING,
                   "Dolby Vision RPU input problem: epoch %u, input seq %u, "
                   "pts_us %"PRId64", rpu %u (%u bytes, hash %08x), "
                   "profile %u, %s\n",
                   d->epoch, seq, entry->pts_us, entry->rpu_count,
                   entry->rpu_size, entry->rpu_hash, d->dovi.cfg.dv_profile,
                   p5_rpu_status_names[status]);
    }
}

void ff_mediacodec_dovi_track_input(AVCodecContext *avctx,
                                    FFMediacodecDovi *dovi,
                                    const AVPacket *pkt, int64_t pts_us)
{
    P5RpuEntry entry = { 0 };

    if (!dovi)
        return;

    p5_rpu_parse_packet(avctx, dovi, &entry, pkt, pts_us);
    p5_rpu_commit(avctx, dovi, &entry);
}

void ff_mediacodec_dovi_attach_output(AVCodecContext *avctx,
                                      FFMediacodecDovi *dovi,
                                      AVFrame *frame, int64_t pts_us)
{
    P5RpuStatus status;
    P5RpuEntry *entry = NULL;
    int found = -1, unconsumed = 0, consumed = 0;
    int metadata_size = 0, attached = 0, input_seq = -1;

    if (!dovi)
        return;

    dovi->outputs++;

    for (unsigned i = 0; i < P5_RPU_MAX_ENTRIES; i++) {
        P5RpuEntry *e = &dovi->entries[i];

        if (e->valid && e->epoch == dovi->epoch && !e->missing_pts &&
            pts_us != AV_NOPTS_VALUE && e->pts_us == pts_us) {
            if (e->consumed) {
                consumed++;
            } else {
                unconsumed++;
                found = i;
            }
        }
    }

    if (pts_us == AV_NOPTS_VALUE)
        status = P5_RPU_MISSING_OUTPUT_PTS;
    else if (unconsumed > 1) {
        /* Several unconsumed access units share this pts, so the frame cannot
         * be attributed; tombstone them to flag any further frame with it. */
        status = P5_RPU_DUPLICATE_INPUT_PTS;
        for (unsigned i = 0; i < P5_RPU_MAX_ENTRIES; i++) {
            P5RpuEntry *ambiguous = &dovi->entries[i];

            if (ambiguous->valid && !ambiguous->consumed &&
                ambiguous->epoch == dovi->epoch && !ambiguous->missing_pts &&
                ambiguous->pts_us == pts_us) {
                ambiguous->consumed = 1;
                p5_rpu_clear_entry(ambiguous);
            }
        }
    } else if (unconsumed == 1) {
        entry = &dovi->entries[found];
        metadata_size = entry->metadata_size;
        input_seq = (int)entry->seq;

        if (entry->duplicate_pts)
            status = P5_RPU_DUPLICATE_INPUT_PTS;
        else {
            status = P5_RPU_MATCH;
            if (!entry->metadata || !entry->raw_rpu)
                status = P5_RPU_MISSING_PARSED_METADATA;
            else if (av_frame_get_side_data(frame, AV_FRAME_DATA_DOVI_METADATA) ||
                     av_frame_get_side_data(frame, AV_FRAME_DATA_DOVI_RPU_BUFFER))
                status = P5_RPU_EXISTING_DOVI_SIDE_DATA;
            else {
                AVFrameSideData *meta_sd, *raw_sd;

                meta_sd = av_frame_new_side_data(frame,
                                                 AV_FRAME_DATA_DOVI_METADATA,
                                                 entry->metadata_size);
                raw_sd = meta_sd ?
                    av_frame_new_side_data(frame, AV_FRAME_DATA_DOVI_RPU_BUFFER,
                                           entry->raw_rpu_size) : NULL;
                if (!meta_sd || !raw_sd) {
                    if (meta_sd)
                        av_frame_remove_side_data(frame,
                                                  AV_FRAME_DATA_DOVI_METADATA);
                    status = P5_RPU_ATTACH_ERROR;
                } else {
                    memcpy(meta_sd->data, entry->metadata,
                           entry->metadata_size);
                    memcpy(raw_sd->data, entry->raw_rpu, entry->raw_rpu_size);
                    attached = 1;
                }
            }
        }
        entry->consumed = 1;
        p5_rpu_clear_entry(entry);
    } else
        status = consumed ? P5_RPU_DUPLICATE_OUTPUT_PTS
                          : P5_RPU_UNMATCHED_OUTPUT_PTS;

    if (status == P5_RPU_MATCH)
        dovi->matched++;
    else {
        dovi->output_errors++;
        if (dovi->output_errors <= 20 || dovi->output_errors % 1000 == 0)
            av_log(avctx, AV_LOG_WARNING,
                   "Dolby Vision RPU output problem: epoch %u, output seq %u, "
                   "pts_us %"PRId64", input seq %d, metadata %d bytes, "
                   "attached %d, %s\n",
                   dovi->epoch, dovi->outputs - 1, pts_us, input_seq,
                   metadata_size, attached, p5_rpu_status_names[status]);
    }
}

void ff_mediacodec_dovi_flush(AVCodecContext *avctx, FFMediacodecDovi *dovi)
{
    if (!dovi)
        return;

    for (unsigned i = 0; i < P5_RPU_MAX_ENTRIES; i++) {
        P5RpuEntry *entry = &dovi->entries[i];

        if (entry->valid && !entry->consumed)
            dovi->discarded++;
        p5_rpu_clear_entry(entry);
        entry->valid = 0;
    }
    av_log(avctx, AV_LOG_VERBOSE,
           "Dolby Vision RPU tracking flushed (epoch %u, %u entries "
           "discarded in total).\n",
           dovi->epoch, dovi->discarded);
    ff_dovi_ctx_flush(&dovi->dovi);
    dovi->epoch++;
}

int ff_mediacodec_dovi_alloc(AVCodecContext *avctx, FFMediacodecDovi **pdovi,
                             int mode)
{
    const AVPacketSideData *sd;
    FFMediacodecDovi *dovi;

    *pdovi = NULL;

    if (avctx->codec_id != AV_CODEC_ID_HEVC || mode == FF_MEDIACODEC_DOVI_OFF)
        return 0;

    sd = ff_get_coded_side_data(avctx, AV_PKT_DATA_DOVI_CONF);
    if (mode == FF_MEDIACODEC_DOVI_AUTO &&
        (!sd || sd->size < sizeof(AVDOVIDecoderConfigurationRecord) ||
         ((const AVDOVIDecoderConfigurationRecord *)sd->data)->dv_profile != 5))
        return 0;

    dovi = av_mallocz(sizeof(*dovi));
    if (!dovi)
        return AVERROR(ENOMEM);
    dovi->entries = av_calloc(P5_RPU_MAX_ENTRIES, sizeof(*dovi->entries));
    if (!dovi->entries) {
        av_free(dovi);
        return AVERROR(ENOMEM);
    }

    dovi->dovi.logctx = avctx;
    if (sd && sd->size >= sizeof(dovi->dovi.cfg))
        memcpy(&dovi->dovi.cfg, sd->data, sizeof(dovi->dovi.cfg));

    av_log(avctx, AV_LOG_VERBOSE,
           "Dolby Vision RPU export enabled (profile %u).\n",
           dovi->dovi.cfg.dv_profile);
    *pdovi = dovi;
    return 0;
}

void ff_mediacodec_dovi_free(AVCodecContext *avctx, FFMediacodecDovi **pdovi)
{
    FFMediacodecDovi *dovi = *pdovi;
    unsigned unconsumed = 0;

    if (!dovi)
        return;

    for (unsigned i = 0; i < P5_RPU_MAX_ENTRIES; i++) {
        P5RpuEntry *entry = &dovi->entries[i];

        if (entry->valid && !entry->consumed)
            unconsumed++;
        p5_rpu_clear_entry(entry);
    }
    av_log(avctx, AV_LOG_VERBOSE,
           "Dolby Vision RPU summary: inputs %u, outputs %u, matched %u, "
           "input errors %u, output errors %u, discarded %u, unconsumed %u, "
           "epoch %u.\n",
           dovi->inputs, dovi->outputs, dovi->matched, dovi->input_errors,
           dovi->output_errors, dovi->discarded, unconsumed, dovi->epoch);

    av_freep(&dovi->entries);
    av_freep(&dovi->rbsp.rbsp_buffer);
    ff_dovi_ctx_unref(&dovi->dovi);
    av_freep(pdovi);
}
