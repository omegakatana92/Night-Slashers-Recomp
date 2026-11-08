/*
 * Night Slashers native runtime: save-state format.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef NSL_STATE_H
#define NSL_STATE_H

#include <stddef.h>
#include <stdint.h>

#define NSL_STATE_MAGIC "NSLSTATE"
#define NSL_STATE_VERSION 1u

/* Fixed header, little-endian, stored uncompressed so the menu can show slot
 * information without decoding the payload. */
typedef struct NslStateHeader {
    char magic[8];                 /* "NSLSTATE" */
    uint32_t version;              /* NSL_STATE_VERSION */
    uint32_t header_size;          /* sizeof(NslStateHeader) */
    char game[16];                 /* "nslasher" */
    uint32_t program_crc;          /* translated program identity */
    uint32_t rom_fingerprint;      /* CRC32 of every ROM region */
    uint32_t layout_cpu;           /* sizeof(NslCpu) */
    uint32_t layout_machine;       /* sizeof(NslMachine) */
    uint32_t layout_sound;         /* nsl_sound_state_size() */
    uint32_t payload_size;         /* uncompressed */
    uint32_t compressed_size;
    uint32_t payload_crc;          /* CRC32 of the uncompressed payload */
    uint64_t frame_number;
    int64_t timestamp;             /* seconds since 1970, set by the front end */
    uint32_t header_crc;           /* CRC32 of this header with header_crc = 0 */
    uint32_t reserved;
} NslStateHeader;

/* Reads and validates only the header (for slot lists). */
int nsl_state_peek(const uint8_t *buffer, size_t size, NslStateHeader *header, char *error, size_t error_size);
void nsl_state_set_timestamp(uint8_t *buffer, size_t size, int64_t timestamp);
/* Extracts the stored screen (320x240 0x00RRGGBB) for slot previews. */
int nsl_state_preview(const uint8_t *buffer, size_t size, uint32_t *pixels);
/* 0 = loadable by this build, otherwise writes why not. */
int nsl_state_check(const uint8_t *buffer, size_t size, char *error, size_t error_size);

#endif
