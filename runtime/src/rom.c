/*
 * Night Slashers native runtime: player ROM loading and region building.
 *
 * File list and CRC32 values: MAME src/mame/dataeast/fghthist.cpp ROM_START(nslasher)
 * (BSD-3-Clause). DECO156 program decryption: MAME deco156.cpp, ported via
 * tools/nsl_rom.py, which was checked word for word against MAME's decrypted image.
 * Tile decryption: MAME decocrpt.cpp (tables in deco_gfx_tables.h).
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "nsl/rom.h"
#include "nsl/platform.h"
#include "deco_gfx_tables.h"
#include "miniz.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct RomFile {
    const char *name;
    uint32_t size;
    uint32_t crc;
} RomFile;

static const RomFile k_files[] = {
    { "mainprg.1f", 0x80000,  0x507ACBAEu },
    { "mainprg.2f", 0x80000,  0x931FC7EEu },
    { "sndprg.17l", 0x10000,  0x18939E92u },
    { "mbh-00.8c",  0x200000, 0xA877F8A3u },
    { "mbh-01.9c",  0x200000, 0x1853DAFCu },
    { "mbh-02.14c", 0x200000, 0xB2F158A1u },
    { "mbh-04.16c", 0x200000, 0xEECFE06Du },
    { "mbh-06.18c", 0x100000, 0x038C2127u },
    { "mbh-03.15c", 0x080000, 0x787787E3u },
    { "mbh-05.17c", 0x080000, 0x1D2B7C17u },
    { "mbh-07.19c", 0x040000, 0xBBD22323u },
    { "mbh-08.16e", 0x080000, 0xCDD7F8CBu },
    { "mbh-09.18e", 0x080000, 0x33FA2121u },
    { "mbh-10.14l", 0x080000, 0xC4D6B116u },
    { "mbh-11.16l", 0x080000, 0x0EC40B6Bu },
};
enum { FILE_COUNT = sizeof(k_files) / sizeof(k_files[0]) };

/* Program ROMs of the Japan clone: recognised so the error can say what it is. */
static const RomFile k_japan_program[] = {
    { "lx-00.1f", 0x80000, 0x6ED5FB88u },
    { "lx-01.2f", 0x80000, 0xA6DF2152u },
};

uint32_t nsl_crc32(const void *data, size_t size) {
    return (uint32_t)mz_crc32(MZ_CRC32_INIT, (const unsigned char *)data, size);
}

/* ---- source access: zip archive or directory ---------------------------- */

typedef struct RomSource {
    int is_zip;
    mz_zip_archive zip;
    uint8_t *zip_data;
    char directory[1024];
} RomSource;

static int ends_with_ci(const char *text, const char *suffix) {
    size_t a = strlen(text), b = strlen(suffix);
    if (b > a) return 0;
    for (size_t i = 0; i < b; i++) {
        char x = text[a - b + i], y = suffix[i];
        if (x >= 'A' && x <= 'Z') x = (char)(x - 'A' + 'a');
        if (x != y) return 0;
    }
    return 1;
}

static int names_equal_ci(const char *a, const char *b) {
    for (;; a++, b++) {
        char x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') x = (char)(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = (char)(y - 'A' + 'a');
        if (x != y) return 0;
        if (!x) return 1;
    }
}

static int source_open(RomSource *source, const char *path, char *error, size_t error_size) {
    memset(source, 0, sizeof(*source));
    if (nsl_path_is_directory(path)) {
        snprintf(source->directory, sizeof(source->directory), "%s", path);
        return 0;
    }
    size_t size = 0;
    source->zip_data = nsl_read_file(path, &size);
    if (!source->zip_data) {
        snprintf(error, error_size, "Cannot read \"%s\". Check that the file exists and is readable.", path);
        return -1;
    }
    if (!mz_zip_reader_init_mem(&source->zip, source->zip_data, size, 0)) {
        free(source->zip_data);
        source->zip_data = NULL;
        snprintf(error, error_size, "\"%s\" is not a valid ZIP archive.", path);
        return -1;
    }
    source->is_zip = 1;
    return 0;
}

static void source_close(RomSource *source) {
    if (source->is_zip) {
        mz_zip_reader_end(&source->zip);
    }
    free(source->zip_data);
    source->zip_data = NULL;
}

/* Reads one member by base name (case-insensitive, any folder inside the zip). */
static uint8_t *source_read(RomSource *source, const char *name, size_t *size) {
    if (!source->is_zip) {
        char full[1200];
        snprintf(full, sizeof(full), "%s%s%s", source->directory,
                 ends_with_ci(source->directory, "/") || ends_with_ci(source->directory, "\\") ? "" : "/",
                 name);
        return nsl_read_file(full, size);
    }
    mz_uint count = mz_zip_reader_get_num_files(&source->zip);
    for (mz_uint index = 0; index < count; index++) {
        mz_zip_archive_file_stat stat;
        if (!mz_zip_reader_file_stat(&source->zip, index, &stat) || stat.m_is_directory) {
            continue;
        }
        const char *base = stat.m_filename;
        for (const char *p = stat.m_filename; *p; p++) {
            if (*p == '/' || *p == '\\') base = p + 1;
        }
        if (names_equal_ci(base, name)) {
            return (uint8_t *)mz_zip_reader_extract_to_heap(&source->zip, index, size, 0);
        }
    }
    return NULL;
}

/* ---- DECO156 program decryption (MAME deco156.cpp) ---------------------- */

static const unsigned char k_bs[4][32] = {
    { 1, 4, 7, 28, 22, 18, 20, 9, 16, 10, 30, 2, 31, 24, 19, 29,
      6, 21, 23, 11, 12, 13, 5, 0, 8, 26, 27, 15, 14, 17, 25, 3 },
    { 14, 23, 28, 29, 6, 24, 10, 1, 5, 16, 7, 2, 30, 8, 18, 3,
      31, 22, 25, 20, 17, 0, 19, 27, 9, 12, 21, 15, 26, 13, 4, 11 },
    { 19, 30, 21, 4, 2, 18, 15, 1, 12, 25, 8, 0, 24, 20, 17, 23,
      22, 26, 28, 16, 9, 27, 6, 11, 31, 10, 3, 13, 14, 7, 29, 5 },
    { 30, 6, 15, 0, 31, 18, 26, 22, 14, 23, 19, 17, 10, 8, 11, 20,
      1, 28, 2, 4, 9, 24, 25, 27, 7, 21, 13, 29, 5, 3, 16, 12 },
};
static const uint32_t k_xor[4] = { 0xEC63197Au, 0x58A5A55Fu, 0xE3A65F16u, 0x28D93783u };
static const uint32_t k_addr_xor[16][2] = {
    { 0x0001, 0xCE4A }, { 0x0002, 0x4DB2 }, { 0x0004, 0xEF60 }, { 0x0008, 0x5737 },
    { 0x0010, 0x13DC }, { 0x0020, 0x4BD9 }, { 0x0040, 0xA209 }, { 0x0080, 0xD996 },
    { 0x0100, 0xA700 }, { 0x0200, 0xECA0 }, { 0x0400, 0x7529 }, { 0x0800, 0x3100 },
    { 0x1000, 0x33B4 }, { 0x2000, 0x6161 }, { 0x4000, 0x1EEF }, { 0x8000, 0xF5A5 },
};
static const uint32_t k_data_xor[16][2] = {
    { 0x00004, 0x04400000 }, { 0x00008, 0x40000004 }, { 0x00010, 0x00048000 },
    { 0x00020, 0x00000280 }, { 0x00040, 0x00200040 }, { 0x00080, 0x09000000 },
    { 0x00100, 0x00001100 }, { 0x00200, 0x20002000 }, { 0x00400, 0x00000022 },
    { 0x00800, 0x000A0000 }, { 0x01000, 0x10004000 }, { 0x02000, 0x00010400 },
    { 0x04000, 0x80000010 }, { 0x08000, 0x00000009 }, { 0x10000, 0x02100000 },
    { 0x20000, 0x00800800 },
};

static void deco156_decrypt(const uint8_t *one, const uint8_t *two, uint8_t *out) {
    enum { WORDS = 0x40000 };
    uint32_t *source = (uint32_t *)malloc(WORDS * sizeof(uint32_t));
    for (uint32_t index = 0; index < WORDS; index++) {
        uint32_t offset = index * 2;     /* MAME ROM_LOAD32_WORD */
        source[index] = (uint32_t)one[offset] | ((uint32_t)one[offset + 1] << 8)
                      | ((uint32_t)two[offset] << 16) | ((uint32_t)two[offset + 1] << 24);
    }
    for (uint32_t index = 0; index < WORDS; index++) {
        uint32_t address = (index & 0xFF0000u) | 0x92C6u;
        for (int k = 0; k < 16; k++) {
            if (index & k_addr_xor[k][0]) address ^= k_addr_xor[k][1];
        }
        uint32_t value = source[address];
        for (int k = 0; k < 16; k++) {
            if (index & k_data_xor[k][0]) value ^= k_data_xor[k][1];
        }
        value ^= k_xor[index & 3];
        uint32_t result = 0;
        for (int k = 0; k < 32; k++) {
            result = (result << 1) | ((value >> k_bs[index & 3][k]) & 1u);
        }
        out[index * 4 + 0] = (uint8_t)result;
        out[index * 4 + 1] = (uint8_t)(result >> 8);
        out[index * 4 + 2] = (uint8_t)(result >> 16);
        out[index * 4 + 3] = (uint8_t)(result >> 24);
    }
    free(source);
}

/* ---- tile ROMs: reorder_tilegfx + deco_decrypt (MAME decocrpt.cpp) ------- */

static void reorder_tilegfx(uint8_t *ram) {
    uint8_t *temp = (uint8_t *)malloc(0x80000);
    memcpy(temp, ram + 0x080000, 0x80000);
    memcpy(ram + 0x080000, ram + 0x100000, 0x80000);
    memcpy(ram + 0x100000, temp, 0x80000);
    free(temp);
}

static void deco_decrypt(uint8_t *ram, uint32_t bytes, const uint8_t *xor_table,
                         const uint16_t *address_table, const uint8_t *swap_table) {
    uint32_t len = bytes / 2;
    uint16_t *buffer = (uint16_t *)malloc(bytes);
    for (uint32_t i = 0; i < len; i++) {       /* big-endian 16-bit words */
        buffer[i] = (uint16_t)((ram[i * 2] << 8) | ram[i * 2 + 1]);
    }
    for (uint32_t i = 0; i < len; i++) {
        uint32_t addr = (i & ~0x7FFu) | address_table[i & 0x7FF];
        const uint8_t *pattern = swap_patterns[swap_table[i & 0x7FF]];
        uint16_t value = (uint16_t)(buffer[addr] ^ xor_masks[xor_table[addr & 0x7FF]]);
        uint16_t result = 0;
        for (int k = 0; k < 16; k++) {
            result = (uint16_t)((result << 1) | ((value >> pattern[k]) & 1u));
        }
        ram[i * 2] = (uint8_t)(result >> 8);
        ram[i * 2 + 1] = (uint8_t)result;
    }
    free(buffer);
}

/* ---- sprites1: ROM_LOAD40_WORD_SWAP / ROM_LOAD40_BYTE ------------------- */

static void load40_word_swap(uint8_t *region, uint32_t offset, const uint8_t *data, uint32_t size) {
    for (uint32_t k = 0; k < size / 2; k++) {
        region[offset + k * 5 + 0] = data[k * 2 + 1];
        region[offset + k * 5 + 1] = data[k * 2 + 0];
    }
}

static void load40_byte(uint8_t *region, uint32_t offset, const uint8_t *data, uint32_t size) {
    for (uint32_t k = 0; k < size; k++) {
        region[offset + k * 5] = data[k];
    }
}

/* ---- public ------------------------------------------------------------- */

void nsl_rom_free(NslRomImages *images) {
    free(images->program);
    free(images->tiles1);
    free(images->tiles2);
    free(images->sprites1);
    free(images->sprites2);
    free(images->sound);
    free(images->oki1);
    free(images->oki2);
    memset(images, 0, sizeof(*images));
}

static int read_all(RomSource *source, uint8_t **data, size_t *sizes, char *error, size_t error_size) {
    char missing[512] = "", wrong[512] = "";
    int bad = 0;
    for (int index = 0; index < FILE_COUNT; index++) {
        data[index] = source_read(source, k_files[index].name, &sizes[index]);
        if (!data[index]) {
            size_t used = strlen(missing);
            snprintf(missing + used, sizeof(missing) - used, "%s%s", used ? ", " : "", k_files[index].name);
            bad = 1;
            continue;
        }
        if (sizes[index] != k_files[index].size || nsl_crc32(data[index], sizes[index]) != k_files[index].crc) {
            size_t used = strlen(wrong);
            snprintf(wrong + used, sizeof(wrong) - used, "%s%s", used ? ", " : "", k_files[index].name);
            bad = 1;
        }
    }
    if (bad) {
        int japan = 0;
        for (int k = 0; k < 2; k++) {
            size_t size = 0;
            uint8_t *probe = source_read(source, k_japan_program[k].name, &size);
            if (probe) japan = 1;
            free(probe);
        }
        snprintf(error, error_size,
                 "This is not a complete Night Slashers (nslasher, Korea Ver. 1.3) set.%s%s%s%s%s",
                 missing[0] ? "\nMissing: " : "", missing,
                 wrong[0] ? "\nWrong size or CRC (bad dump or another revision): " : "", wrong,
                 japan ? "\nThis looks like the Japan set (nslasherj); this build was translated "
                         "from the Korea program and needs nslasher.zip." : "");
        return -1;
    }
    return 0;
}

int nsl_rom_load(const char *path, NslRomImages *images, char *error, size_t error_size) {
    memset(images, 0, sizeof(*images));
    if (!path || !path[0]) {
        snprintf(error, error_size, "No ROM selected.");
        return -1;
    }
    RomSource source;
    if (source_open(&source, path, error, error_size) != 0) {
        return -1;
    }
    uint8_t *data[FILE_COUNT];
    size_t sizes[FILE_COUNT];
    int result = read_all(&source, data, sizes, error, error_size);
    source_close(&source);
    if (result != 0) {
        for (int index = 0; index < FILE_COUNT; index++) free(data[index]);
        return -1;
    }

    snprintf(images->set_name, sizeof(images->set_name), "nslasher");
    snprintf(images->title, sizeof(images->title), "Night Slashers (Korea Ver. 1.3, DE-0397-0 PCB)");
    images->program = (uint8_t *)malloc(0x100000);
    deco156_decrypt(data[0], data[1], images->program);
    images->program_crc = nsl_crc32(images->program, 0x100000);

    images->sound = data[2];
    data[2] = NULL;

    images->tiles1 = data[3];
    images->tiles2 = data[4];
    data[3] = data[4] = NULL;
    reorder_tilegfx(images->tiles1);
    reorder_tilegfx(images->tiles2);
    deco_decrypt(images->tiles1, NSL_TILES_SIZE, deco56_xor_table, deco56_address_table, deco56_swap_table);
    deco_decrypt(images->tiles2, NSL_TILES_SIZE, deco74_xor_table, deco74_address_table, deco74_swap_table);

    images->sprites1 = (uint8_t *)calloc(1, NSL_SPRITES1_SIZE);
    load40_word_swap(images->sprites1, 0x000003, data[5], 0x200000);   /* mbh-02 */
    load40_word_swap(images->sprites1, 0x000001, data[6], 0x200000);   /* mbh-04 */
    load40_byte(images->sprites1, 0x000000, data[7], 0x100000);        /* mbh-06 */
    load40_word_swap(images->sprites1, 0x500003, data[8], 0x080000);   /* mbh-03 */
    load40_word_swap(images->sprites1, 0x500001, data[9], 0x080000);   /* mbh-05 */
    load40_byte(images->sprites1, 0x500000, data[10], 0x040000);       /* mbh-07 */

    images->sprites2 = (uint8_t *)malloc(NSL_SPRITES2_SIZE);
    memcpy(images->sprites2, data[11], 0x80000);
    memcpy(images->sprites2 + 0x80000, data[12], 0x80000);

    images->oki1 = data[13];
    images->oki2 = data[14];
    data[13] = data[14] = NULL;
    for (int index = 0; index < FILE_COUNT; index++) free(data[index]);
    return 0;
}

int nsl_rom_identify(const char *path, char *summary, size_t summary_size) {
    RomSource source;
    if (source_open(&source, path, summary, summary_size) != 0) {
        return -1;
    }
    uint8_t *data[FILE_COUNT];
    size_t sizes[FILE_COUNT];
    int result = read_all(&source, data, sizes, summary, summary_size);
    source_close(&source);
    for (int index = 0; index < FILE_COUNT; index++) free(data[index]);
    if (result == 0) {
        snprintf(summary, summary_size, "Night Slashers (Korea Ver. 1.3) - all %d files verified", FILE_COUNT);
    }
    return result;
}
