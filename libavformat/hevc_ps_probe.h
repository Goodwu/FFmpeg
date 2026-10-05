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

#ifndef AVFORMAT_HEVC_PS_PROBE_H
#define AVFORMAT_HEVC_PS_PROBE_H

#include <stddef.h>
#include <stdint.h>

typedef struct FFHEVCPSProbe FFHEVCPSProbe;

/* Allocation-free structural/PS-ID preflight: 1 missing type, 0 all types, <0 malformed.
 * Like init, all types means no repair; it does not claim valid reference chains. */
int ff_hevc_ps_probe_missing_types(const uint8_t *hvcc, size_t size);

/* 1: missing PS, context created; 0: complete, unchanged; <0: fail closed. */
int ff_hevc_ps_probe_init(FFHEVCPSProbe **probe, const uint8_t *hvcc,
                          size_t size, size_t scan_budget, size_t storage_budget);
/* Consume a complete length-prefixed packet read-only. 1: complete chain. */
int ff_hevc_ps_probe_feed(FFHEVCPSProbe *probe, const uint8_t *data, size_t size);
int ff_hevc_ps_probe_pending(const FFHEVCPSProbe *probe);
int ff_hevc_ps_probe_matches(const FFHEVCPSProbe *probe,
                             const uint8_t *hvcc, size_t size);
/* Allocate a padded replacement. Caller publishes only after successful build. */
int ff_hevc_ps_probe_build(FFHEVCPSProbe *probe, uint8_t **hvcc, int *size);
void ff_hevc_ps_probe_free(FFHEVCPSProbe **probe);

#endif
