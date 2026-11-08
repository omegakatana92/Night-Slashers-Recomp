/*
 * Night Slashers native runtime: game core API.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "nsl/core.h"
#include "nsl/cpu.h"
#include "nsl/rom.h"
#include "nsl/run.h"
#include "nsl/sound.h"
#include "miniz.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static NslRomImages g_rom;
static int g_loaded;
static uint32_t g_fingerprint;

int nsl_core_loaded(void) { return g_loaded; }
const char *nsl_core_rom_title(void) { return g_loaded ? g_rom.title : ""; }
uint32_t nsl_core_rom_fingerprint(void) { return g_fingerprint; }
const char *nsl_core_fault_text(void) { return nsl_cpu.fault_text; }
uint64_t nsl_core_frame_number(void) { return nsl_machine.frame_count; }
const uint32_t *nsl_core_framebuffer(void) { return &nsl_host.framebuffer[0][0]; }

void nsl_core_unload(void) {
    if (!g_loaded) return;
    nsl_sound_shutdown();
    nsl_video_free_gfx();
    nsl_rom_free(&g_rom);
    g_loaded = 0;
}

int nsl_core_load_rom(const char *path, char *error, size_t error_size) {
    NslRomImages images;
    if (nsl_rom_load(path, &images, error, error_size) != 0) {
        return -1;
    }
    if (images.program_crc != nsl_translated_program_crc32) {
        snprintf(error, error_size,
                 "The program ROMs do not match the translated program in this build "
                 "(CRC %08X, expected %08X for %s).",
                 images.program_crc, nsl_translated_program_crc32, nsl_translated_program_set);
        nsl_rom_free(&images);
        return -1;
    }
    nsl_core_unload();
    g_rom = images;
    if (nsl_video_attach_gfx(g_rom.tiles1, g_rom.tiles2, g_rom.sprites1, g_rom.sprites2) != 0) {
        snprintf(error, error_size, "Out of memory while decoding graphics.");
        nsl_rom_free(&g_rom);
        return -1;
    }
    uint32_t crc = (uint32_t)mz_crc32(MZ_CRC32_INIT, g_rom.program, 0x100000);
    crc = (uint32_t)mz_crc32(crc, g_rom.tiles1, NSL_TILES_SIZE);
    crc = (uint32_t)mz_crc32(crc, g_rom.tiles2, NSL_TILES_SIZE);
    crc = (uint32_t)mz_crc32(crc, g_rom.sprites1, NSL_SPRITES1_SIZE);
    crc = (uint32_t)mz_crc32(crc, g_rom.sprites2, NSL_SPRITES2_SIZE);
    crc = (uint32_t)mz_crc32(crc, g_rom.sound, NSL_SOUND_SIZE);
    crc = (uint32_t)mz_crc32(crc, g_rom.oki1, NSL_OKI_SIZE);
    crc = (uint32_t)mz_crc32(crc, g_rom.oki2, NSL_OKI_SIZE);
    g_fingerprint = crc;
    nsl_machine_init(g_rom.program);
    nsl_sound_init(g_rom.sound, g_rom.oki1, g_rom.oki2);
    nsl_cpu_reset();
    g_loaded = 1;
    return 0;
}

void nsl_core_reset(void) {
    nsl_machine_reset();
    nsl_sound_reset();
    nsl_cpu_reset();
}

static void apply_inputs(const NslInputState *input) {
    NslInputState none;
    if (!input) { memset(&none, 0, sizeof(none)); input = &none; }
    uint16_t in0 = 0, in1 = 0;
    in0 |= (uint16_t)(input->player[0] & 0xFF);
    in0 |= (uint16_t)((input->player[1] & 0xFF) << 8);
    if (input->player[0] & NSL_IN_COIN) in1 |= 0x0001;
    if (input->player[1] & NSL_IN_COIN) in1 |= 0x0002;
    if (input->system & NSL_SYS_SERVICE_COIN) in1 |= 0x0004;
    if (input->system & NSL_SYS_TEST) in1 |= 0x0008;
    in1 |= (uint16_t)((input->player[2] & 0xFF) << 8);
    /* active low; bits 5-7 of IN1 are unused (read as 1) */
    nsl_machine_set_inputs((uint16_t)~in0, (uint16_t)~in1);
}

int nsl_core_run_frame(const NslInputState *input) {
    if (!g_loaded) return -1;
    apply_inputs(input);
    nsl_sound_begin_frame();
    int result = nsl_run_frame();
    nsl_sound_end_frame();
    return result;
}

int nsl_core_audio_rate(void) { return nsl_sound_output_rate(); }
size_t nsl_core_audio(const int16_t **samples) { return nsl_sound_output(samples); }

void nsl_core_get_eeprom(uint8_t out[128]) { nsl_machine_get_eeprom(out); }
void nsl_core_set_eeprom(const uint8_t in[128]) { nsl_machine_set_eeprom(in); }

int nsl_core_png_encode(const uint32_t *pixels, int width, int height, uint8_t **png, size_t *png_size) {
    uint8_t *rgb = (uint8_t *)malloc((size_t)width * height * 3);
    if (!rgb) return -1;
    for (int i = 0; i < width * height; i++) {
        rgb[i * 3 + 0] = (uint8_t)(pixels[i] >> 16);
        rgb[i * 3 + 1] = (uint8_t)(pixels[i] >> 8);
        rgb[i * 3 + 2] = (uint8_t)pixels[i];
    }
    size_t size = 0;
    void *data = tdefl_write_image_to_png_file_in_memory_ex(rgb, width, height, 3, &size, 6, MZ_FALSE);
    free(rgb);
    if (!data) return -1;
    *png = (uint8_t *)data;
    *png_size = size;
    return 0;
}
