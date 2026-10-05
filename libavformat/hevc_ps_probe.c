/*
 * Copyright (c) 2014 Tim Walker <tdskywalker@gmail.com>
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

#include <limits.h>
#include <string.h>

#include "libavcodec/defs.h"
#include "libavutil/error.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/mem.h"
#include "hevc_ps_probe.h"

#define MAX_VPS 16
#define MAX_SPS 16
#define MAX_PPS 64

typedef struct PS {
    uint8_t *data;
    uint16_t size;
    uint8_t parent;
} PS;

struct FFHEVCPSProbe {
    uint8_t *original;
    size_t original_size;
    size_t scan_left;
    size_t storage_left;
    int length_size;
    int failed;
    int complete;
    PS vps[MAX_VPS];
    PS sps[MAX_SPS];
    PS pps[MAX_PPS];
};

typedef struct PSBits {
    const uint8_t *data;
    size_t bits;
    size_t pos;
    int error;
} PSBits;

static unsigned read_bits(PSBits *b, unsigned n)
{
    unsigned value = 0;
    if (n > 32 || b->pos > b->bits || n > b->bits - b->pos) {
        b->error = 1;
        return 0;
    }
    while (n--) {
        value = (value << 1) | ((b->data[b->pos / 8] >> (7 - b->pos % 8)) & 1);
        b->pos++;
    }
    return value;
}

static void skip_bits(PSBits *b, unsigned n)
{
    while (n > 32) {
        read_bits(b, 32);
        n -= 32;
    }
    read_bits(b, n);
}

static unsigned read_ue(PSBits *b)
{
    unsigned zeros = 0, suffix;
    while (!b->error && !read_bits(b, 1)) {
        if (++zeros > 30) {
            b->error = 1;
            return 0;
        }
    }
    suffix = read_bits(b, zeros);
    return ((1U << zeros) - 1) + suffix;
}

/* Parse only IDs and reference edges, not a software decoder or full PS syntax. */
static int ps_ids_into(const uint8_t *nal, size_t size, int type,
                   unsigned *id, unsigned *parent, uint8_t *rbsp, size_t capacity)
{
    size_t length = 0;
    int zeros = 0, ret = AVERROR_INVALIDDATA;
    PSBits b;

    if (size < 3 || size > UINT16_MAX || (nal[0] & 0x81) ||
        nal[1] != 1 || ((nal[0] >> 1) & 63) != type)
        return ret; /* only layer zero, temporal_id_plus1=1 */
    for (size_t i = 2; i < size; i++) {
        uint8_t byte = nal[i];
        if (zeros >= 2) {
            if (byte == 3) {
                if (i + 1 == size || nal[i + 1] > 3)
                    goto end;
                zeros = 0;
                continue;
            }
            if (byte < 3)
                goto end;
        }
        if (length < capacity)
            rbsp[length] = byte;
        length++;
        zeros = byte ? 0 : zeros + 1;
    }
    b = (PSBits){ .data = rbsp, .bits = FFMIN(length, capacity) * 8 };
    *parent = 0;
    if (type == 32) {
        *id = read_bits(&b, 4);
    } else if (type == 33) {
        unsigned layers, profile[7] = { 0 }, level[7] = { 0 };
        *parent = read_bits(&b, 4);
        layers = read_bits(&b, 3);
        if (layers > 6)
            goto end;
        read_bits(&b, 1);
        skip_bits(&b, 96); /* general_profile_tier_level */
        for (unsigned i = 0; i < layers; i++) {
            profile[i] = read_bits(&b, 1);
            level[i] = read_bits(&b, 1);
        }
        if (layers)
            for (unsigned i = layers; i < 8; i++)
                if (read_bits(&b, 2))
                    goto end;
        for (unsigned i = 0; i < layers; i++) {
            if (profile[i])
                skip_bits(&b, 88);
            if (level[i])
                skip_bits(&b, 8);
        }
        *id = read_ue(&b);
    } else if (type == 34) {
        *id = read_ue(&b);
        *parent = read_ue(&b);
    } else {
        goto end;
    }
    if (b.error || *id >= (type == 34 ? MAX_PPS : MAX_SPS) ||
        *parent >= (type == 34 ? MAX_SPS : MAX_VPS) || b.pos >= b.bits)
        goto end;
    ret = 0;
end:
    return ret;
}

static int ps_ids(const uint8_t *nal, size_t size, int type,
                   unsigned *id, unsigned *parent)
{
    uint8_t *rbsp;
    int ret;
    if (size < 3 || size > UINT16_MAX || (nal[0] & 0x81) ||
        nal[1] != 1 || ((nal[0] >> 1) & 63) != type)
        return AVERROR_INVALIDDATA;
    rbsp = av_malloc(size - 2);
    if (!rbsp)
        return AVERROR(ENOMEM);
    ret = ps_ids_into(nal, size, type, id, parent, rbsp, size - 2);
    av_free(rbsp);
    return ret;
}

static PS *slot(FFHEVCPSProbe *p, int type, unsigned id)
{
    return type == 32 ? &p->vps[id] : type == 33 ? &p->sps[id] : &p->pps[id];
}

static int store_ps(FFHEVCPSProbe *p, const uint8_t *data, size_t size, int type)
{
    unsigned id, parent;
    PS *s;
    int ret;
    if (size > p->storage_left)
        return AVERROR(ENOSPC);
    ret = ps_ids(data, size, type, &id, &parent);
    if (ret < 0)
        return ret;
    s = slot(p, type, id);
    if (s->data)
        return s->size == size && !memcmp(s->data, data, size) ? 0 : AVERROR_INVALIDDATA;
    if (size > p->storage_left)
        return AVERROR(ENOSPC);
    s->data = av_memdup(data, size);
    if (!s->data)
        return AVERROR(ENOMEM);
    s->size = size;
    s->parent = parent;
    p->storage_left -= size;
    return 0;
}

static int complete_chain(FFHEVCPSProbe *p)
{
    int nv = 0, ns = 0, np = 0;
    for (unsigned i = 0; i < MAX_VPS; i++)
        nv += !!p->vps[i].data;
    for (unsigned i = 0; i < MAX_SPS; i++) {
        if (p->sps[i].data) {
            if (!p->vps[p->sps[i].parent].data)
                return 0;
            ns++;
        }
    }
    for (unsigned i = 0; i < MAX_PPS; i++) {
        if (p->pps[i].data) {
            if (!p->sps[p->pps[i].parent].data)
                return 0;
            np++;
        }
    }
    return nv && ns && np;
}

static int nal_header(const uint8_t *data, size_t size, int type)
{
    return size >= 2 && !(data[0] & 0x80) && (data[1] & 7) &&
           ((data[0] >> 1) & 63) == type;
}

/* Same missing-type scope as init, without allocating repair workspace. */
int ff_hevc_ps_probe_missing_types(const uint8_t *hvcc, size_t size)
{
    /* IDs need at most 108 RBSP bytes: even six sublayer PTLs plus the
     * largest accepted Exp-Golomb ID fit. Validate EPB across the whole NAL,
     * retain only this bounded prefix; no heap or repair budget is needed. */
    uint8_t rbsp[128];
    PS vps[MAX_VPS] = { 0 }, sps[MAX_SPS] = { 0 }, pps[MAX_PPS] = { 0 };
    uint64_t array_types = 0;
    size_t pos = 23;
    int types = 0;

    if (!hvcc || size < 23 || size > INT_MAX - AV_INPUT_BUFFER_PADDING_SIZE ||
        hvcc[0] != 1 || (hvcc[13] & 0xf0) != 0xf0 ||
        (hvcc[15] & 0xfc) != 0xfc || (hvcc[16] & 0xfc) != 0xfc ||
        (hvcc[17] & 0xf8) != 0xf8 || (hvcc[18] & 0xf8) != 0xf8)
        return AVERROR_INVALIDDATA;
    for (unsigned array = 0; array < hvcc[22]; array++) {
        int type;
        unsigned count;
        if (size - pos < 3 || (hvcc[pos] & 0x40))
            return AVERROR_INVALIDDATA;
        type = hvcc[pos] & 63;
        if (array_types & (UINT64_C(1) << type))
            return AVERROR_INVALIDDATA;
        array_types |= UINT64_C(1) << type;
        count = AV_RB16(hvcc + pos + 1);
        pos += 3;
        for (unsigned i = 0; i < count; i++) {
            unsigned length, id, parent;
            int ret;
            PS *slot;
            if (size - pos < 2)
                return AVERROR_INVALIDDATA;
            length = AV_RB16(hvcc + pos);
            pos += 2;
            if (length > size - pos || !nal_header(hvcc + pos, length, type))
                return AVERROR_INVALIDDATA;
            if (type >= 32 && type <= 34) {
                ret = ps_ids_into(hvcc + pos, length, type, &id, &parent, rbsp, sizeof(rbsp));
                if (ret < 0)
                    return ret;
                slot = type == 32 ? &vps[id] : type == 33 ? &sps[id] : &pps[id];
                if (slot->data && (slot->size != length ||
                                  memcmp(slot->data, hvcc + pos, length)))
                    return AVERROR_INVALIDDATA;
                /* Borrowed only within this stack invocation, never retained. */
                slot->data = (uint8_t *)hvcc + pos;
                slot->size = length;
                slot->parent = parent;
                types |= 1 << (type - 32);
            }
            pos += length;
        }
    }
    if (pos != size)
        return AVERROR_INVALIDDATA;
    return types == 7 ? 0 : 1;
}

void ff_hevc_ps_probe_free(FFHEVCPSProbe **probe)
{
    FFHEVCPSProbe *p = *probe;
    if (!p)
        return;
    for (unsigned i = 0; i < MAX_VPS; i++)
        av_free(p->vps[i].data);
    for (unsigned i = 0; i < MAX_SPS; i++)
        av_free(p->sps[i].data);
    for (unsigned i = 0; i < MAX_PPS; i++)
        av_free(p->pps[i].data);
    av_free(p->original);
    av_freep(probe);
}

int ff_hevc_ps_probe_init(FFHEVCPSProbe **probe, const uint8_t *hvcc,
                          size_t size, size_t scan_budget, size_t storage_budget)
{
    FFHEVCPSProbe *p;
    size_t pos = 23;
    int ret = AVERROR_INVALIDDATA;
    uint64_t array_types = 0;

    *probe = NULL;
    if (!hvcc || size < 23 || size > INT_MAX - AV_INPUT_BUFFER_PADDING_SIZE ||
        hvcc[0] != 1 || (hvcc[13] & 0xf0) != 0xf0 ||
        (hvcc[15] & 0xfc) != 0xfc || (hvcc[16] & 0xfc) != 0xfc ||
        (hvcc[17] & 0xf8) != 0xf8 || (hvcc[18] & 0xf8) != 0xf8)
        return ret;
    if (!scan_budget || storage_budget < sizeof(*p) || size > storage_budget - sizeof(*p))
        return AVERROR(ENOSPC);
    p = av_mallocz(sizeof(*p));
    if (!p)
        return AVERROR(ENOMEM);
    p->length_size = (hvcc[21] & 3) + 1;
    p->scan_left = scan_budget;
    p->storage_left = storage_budget - size - sizeof(*p);
    p->original = av_memdup(hvcc, size);
    if (!p->original) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }
    p->original_size = size;
    for (unsigned array = 0; array < hvcc[22]; array++) {
        int type;
        unsigned count;
        if (size - pos < 3 || (hvcc[pos] & 0x40))
            goto invalid;
        type = hvcc[pos] & 63;
        if (array_types & (UINT64_C(1) << type))
            goto invalid;
        array_types |= UINT64_C(1) << type;
        count = AV_RB16(hvcc + pos + 1);
        pos += 3;
        for (unsigned i = 0; i < count; i++) {
            unsigned length;
            if (size - pos < 2)
                goto invalid;
            length = AV_RB16(hvcc + pos);
            pos += 2;
            if (length > size - pos || !nal_header(hvcc + pos, length, type))
                goto invalid;
            if (type >= 32 && type <= 34) {
                ret = store_ps(p, hvcc + pos, length, type);
                if (ret < 0)
                    goto fail;
            }
            pos += length;
        }
    }
    if (pos != size)
        goto invalid;
    {
        int nv = 0, ns = 0, np = 0;
        for (unsigned i = 0; i < MAX_VPS; i++) nv += !!p->vps[i].data;
        for (unsigned i = 0; i < MAX_SPS; i++) ns += !!p->sps[i].data;
        for (unsigned i = 0; i < MAX_PPS; i++) np += !!p->pps[i].data;
        if (nv && ns && np) {
            ff_hevc_ps_probe_free(&p);
            return 0; /* Only a missing PS type is in this repair's scope. */
        }
    }
    *probe = p;
    return 1;
invalid:
    ret = AVERROR_INVALIDDATA;
fail:
    ff_hevc_ps_probe_free(&p);
    return ret;
}

int ff_hevc_ps_probe_feed(FFHEVCPSProbe *p, const uint8_t *data, size_t size)
{
    size_t pos = 0;
    int ret = AVERROR_INVALIDDATA;
    if (!p || p->failed)
        return AVERROR_INVALIDDATA;
    if (size > p->scan_left) {
        p->failed = 1;
        return AVERROR(ENOSPC);
    }
    p->scan_left -= size;
    if (size && !data)
        goto fail;
    /* Validate complete framing before collecting anything from this packet. */
    for (unsigned pass = 0; pass < 2; pass++) {
        pos = 0;
        while (pos < size) {
            uint32_t length = 0;
            int type;
            if (size - pos < (size_t)p->length_size)
                goto fail;
            for (int i = 0; i < p->length_size; i++)
                length = (length << 8) | data[pos++];
            if (length < 2 || length > size - pos)
                goto fail;
            type = (data[pos] >> 1) & 63;
            if (!nal_header(data + pos, length, type))
                goto fail;
            if (pass && type >= 32 && type <= 34) {
                ret = store_ps(p, data + pos, length, type);
                if (ret < 0)
                    goto fail;
            }
            pos += length;
        }
    }
    p->complete = complete_chain(p);
    return p->complete;
fail:
    p->failed = 1;
    return ret < 0 ? ret : AVERROR_INVALIDDATA;
}

int ff_hevc_ps_probe_pending(const FFHEVCPSProbe *p)
{
    return p && !p->failed && !p->complete;
}

int ff_hevc_ps_probe_matches(const FFHEVCPSProbe *p,
                             const uint8_t *hvcc, size_t size)
{
    return p && hvcc && size == p->original_size && !memcmp(hvcc, p->original, size);
}

int ff_hevc_ps_probe_build(FFHEVCPSProbe *p, uint8_t **hvcc, int *size)
{
    size_t total, pos, old_pos = 23;
    unsigned arrays = 3;
    uint8_t *data;
    PS *sets[] = { p ? p->vps : NULL, p ? p->sps : NULL, p ? p->pps : NULL };
    const unsigned counts[] = { MAX_VPS, MAX_SPS, MAX_PPS };

    *hvcc = NULL;
    *size = 0;
    if (!p || p->failed || !p->complete)
        return 0;
    total = 23;
    for (unsigned array = 0; array < p->original[22]; array++) {
        size_t start = old_pos;
        int type = p->original[old_pos] & 63;
        unsigned count = AV_RB16(p->original + old_pos + 1);
        old_pos += 3;
        for (unsigned i = 0; i < count; i++) {
            unsigned length = AV_RB16(p->original + old_pos);
            old_pos += 2 + length;
        }
        if (type < 32 || type > 34) {
            total += old_pos - start;
            arrays++;
        }
    }
    if (arrays > 255)
        return AVERROR_INVALIDDATA;
    for (unsigned t = 0; t < 3; t++) {
        total += 3;
        for (unsigned i = 0; i < counts[t]; i++)
            if (sets[t][i].data)
                total += 2 + sets[t][i].size;
    }
    if (total > INT_MAX - AV_INPUT_BUFFER_PADDING_SIZE ||
        total + AV_INPUT_BUFFER_PADDING_SIZE > p->storage_left)
        return AVERROR(ENOMEM);
    data = av_mallocz(total + AV_INPUT_BUFFER_PADDING_SIZE);
    if (!data)
        return AVERROR(ENOMEM);
    memcpy(data, p->original, 22);
    data[22] = arrays;
    pos = 23;
    old_pos = 23;
    for (unsigned array = 0; array < p->original[22]; array++) {
        size_t start = old_pos;
        int type = p->original[old_pos] & 63;
        unsigned count = AV_RB16(p->original + old_pos + 1);
        old_pos += 3;
        for (unsigned i = 0; i < count; i++)
            old_pos += 2 + AV_RB16(p->original + old_pos);
        if (type < 32 || type > 34) {
            memcpy(data + pos, p->original + start, old_pos - start);
            pos += old_pos - start;
        }
    }
    for (unsigned t = 0; t < 3; t++) {
        unsigned n = 0;
        size_t array_pos = pos;
        data[pos++] = 32 + t; /* Initial observed PS, not a claim about future updates. */
        pos += 2;
        for (unsigned i = 0; i < counts[t]; i++) {
            PS *s = &sets[t][i];
            if (!s->data)
                continue;
            AV_WB16(data + pos, s->size);
            pos += 2;
            memcpy(data + pos, s->data, s->size);
            pos += s->size;
            n++;
        }
        AV_WB16(data + array_pos + 1, n);
    }
    *hvcc = data;
    *size = total;
    return 1;
}
