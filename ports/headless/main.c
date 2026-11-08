/*
 * Night Slashers headless runner: deterministic boot/frame harness and test driver.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "nsl/core.h"
#include "nsl/cpu.h"
#include "nsl/interp.h"
#include "nsl/machine.h"
#include "nsl/platform.h"
#include "nsl/state.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t fnv1a(const void *data, size_t size) {
    const uint8_t *bytes = (const uint8_t *)data;
    uint32_t hash = 2166136261u;
    for (size_t index = 0; index < size; index++) {
        hash ^= bytes[index];
        hash *= 16777619u;
    }
    return hash;
}

static int write_png(const char *path) {
    uint8_t *png = NULL;
    size_t size = 0;
    if (nsl_core_png_encode(nsl_core_framebuffer(), NSL_SCREEN_WIDTH, NSL_SCREEN_HEIGHT, &png, &size) != 0) return -1;
    int result = nsl_write_file_atomic(path, png, size);
    free(png);
    return result;
}

static void usage(const char *name) {
    fprintf(stderr,
            "usage: %s [--rom PATH] [--frames N] [--hashes FILE] [--eeprom FILE] [--save-eeprom FILE]\n"
            "          [--png-every N --png-dir DIR] [--png-at N FILE] [--coin-at N] [--start-at N]\n"
            "          [--save-state-at N FILE] [--load-state FILE] [--trace-eeprom] [--trace-deco]\n"
            "          [--trace-cpu N] [--trace-irq] [--trace-pc ADDR COUNT] [--dump-ram FILE]\n",
            name);
}

int main(int argc, char **argv) {
    int frames = 600;
    const char *rom_path = "rom/nslasher.zip";
    const char *hash_path = NULL, *eeprom_path = NULL, *save_eeprom_path = NULL, *dump_ram_path = NULL;
    const char *wav_path = NULL;
    const char *png_dir = NULL, *png_at_path = NULL, *state_out = NULL, *state_in = NULL;
    int png_every = 0, png_at = -1, state_at = -1;
    int coin_at = -1, start_at = -1;
    int trace_cpu = 0, trace_after = 0, trace_deco = 0, trace_eeprom = 0;
    uint32_t trace_pc = 0;
    int trace_pc_count = 0;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--rom") && i + 1 < argc) rom_path = argv[++i];
        else if (!strcmp(a, "--frames") && i + 1 < argc) frames = atoi(argv[++i]);
        else if (!strcmp(a, "--hashes") && i + 1 < argc) hash_path = argv[++i];
        else if (!strcmp(a, "--eeprom") && i + 1 < argc) eeprom_path = argv[++i];
        else if (!strcmp(a, "--save-eeprom") && i + 1 < argc) save_eeprom_path = argv[++i];
        else if (!strcmp(a, "--dump-ram") && i + 1 < argc) dump_ram_path = argv[++i];
        else if (!strcmp(a, "--wav") && i + 1 < argc) wav_path = argv[++i];
        else if (!strcmp(a, "--png-every") && i + 1 < argc) png_every = atoi(argv[++i]);
        else if (!strcmp(a, "--png-dir") && i + 1 < argc) png_dir = argv[++i];
        else if (!strcmp(a, "--png-at") && i + 2 < argc) { png_at = atoi(argv[++i]); png_at_path = argv[++i]; }
        else if (!strcmp(a, "--save-state-at") && i + 2 < argc) { state_at = atoi(argv[++i]); state_out = argv[++i]; }
        else if (!strcmp(a, "--load-state") && i + 1 < argc) state_in = argv[++i];
        else if (!strcmp(a, "--coin-at") && i + 1 < argc) coin_at = atoi(argv[++i]);
        else if (!strcmp(a, "--start-at") && i + 1 < argc) start_at = atoi(argv[++i]);
        else if (!strcmp(a, "--trace-cpu") && i + 1 < argc) trace_cpu = atoi(argv[++i]);
        else if (!strcmp(a, "--trace-irq")) trace_cpu = -1;
        else if (!strcmp(a, "--trace-deco")) trace_deco = 1;
        else if (!strcmp(a, "--trace-eeprom")) trace_eeprom = 1;
        else if (!strcmp(a, "--trace-after") && i + 1 < argc) trace_after = atoi(argv[++i]);
        else if (!strcmp(a, "--trace-pc") && i + 2 < argc) {
            trace_pc = (uint32_t)strtoul(argv[++i], NULL, 0);
            trace_pc_count = atoi(argv[++i]);
        } else { usage(argv[0]); return 2; }
    }

    char error[1024];
    if (nsl_core_load_rom(rom_path, error, sizeof(error)) != 0) {
        fprintf(stderr, "ROM: %s\n", error);
        return 2;
    }
    if (eeprom_path && nsl_machine_load_eeprom(eeprom_path) != 0) {
        fprintf(stderr, "cannot load EEPROM image %s\n", eeprom_path);
        return 2;
    }
    if (state_in) {
        size_t size = 0;
        uint8_t *data = nsl_read_file(state_in, &size);
        if (!data || nsl_core_load_state(data, size, error, sizeof(error)) != 0) {
            fprintf(stderr, "state %s: %s\n", state_in, data ? error : "cannot read");
            free(data);
            return 3;
        }
        free(data);
    }
    nsl_host.trace_deco = trace_deco;
    nsl_host.trace_eeprom = trace_eeprom;
    nsl_cpu.trace_blocks = trace_cpu > 0 ? trace_cpu : 0;
    nsl_cpu.trace_after = trace_after;
    nsl_cpu.trace_irq = trace_cpu < 0;
    nsl_cpu.trace_pc = trace_pc;
    nsl_cpu.trace_pc_count = trace_pc_count;

    FILE *hashes = hash_path ? nsl_fopen(hash_path, "w") : NULL;
    if (png_dir) nsl_make_directories(png_dir);

    FILE *wav = wav_path ? nsl_fopen(wav_path, "wb") : NULL;
    uint32_t wav_frames = 0;
    if (wav) { uint8_t zero[44] = {0}; fwrite(zero, 1, 44, wav); }
    int rc = 0;
    for (int frame = 0; frame < frames; frame++) {
        NslInputState input;
        memset(&input, 0, sizeof(input));
        if (coin_at >= 0 && frame >= coin_at && frame < coin_at + 6) input.player[0] |= NSL_IN_COIN;
        if (start_at >= 0 && frame >= start_at && frame < start_at + 6) input.player[0] |= NSL_IN_START;
        if (nsl_core_run_frame(&input) != 0) {
            fprintf(stderr, "frame %d: %s\n", frame, nsl_core_fault_text());
            rc = 1;
            break;
        }
        if (wav) {
            const int16_t *samples;
            size_t count = nsl_core_audio(&samples);
            fwrite(samples, 4, count, wav);
            wav_frames += (uint32_t)count;
        }
        if (hashes) {
            const int16_t *audio;
            size_t audio_count = nsl_core_audio(&audio);
            fprintf(hashes, "%d %08X %08X %08X %08X\n", frame, fnv1a(audio, audio_count * 4),
                    fnv1a(nsl_machine.ram, sizeof(nsl_machine.ram)),
                    fnv1a(nsl_host.framebuffer, sizeof(nsl_host.framebuffer)),
                    fnv1a(nsl_machine.palette_buffered, sizeof(nsl_machine.palette_buffered)));
        }
        if (png_every > 0 && png_dir && (frame + 1) % png_every == 0) {
            char path[1024], name[64];
            snprintf(name, sizeof(name), "%06d.png", frame + 1);
            nsl_path_join(path, sizeof(path), png_dir, name);
            write_png(path);
        }
        if (png_at >= 0 && frame + 1 == png_at) write_png(png_at_path);
        if (state_at >= 0 && frame + 1 == state_at) {
            size_t size = nsl_core_state_size();
            uint8_t *data = (uint8_t *)malloc(size);
            int written = data ? nsl_core_save_state(data, size) : -1;
            if (written > 0) nsl_write_file_atomic(state_out, data, (size_t)written);
            else fprintf(stderr, "save state failed\n");
            free(data);
        }
    }
    if (hashes) fclose(hashes);
    if (wav) {
        uint32_t rate = (uint32_t)nsl_core_audio_rate(), data = wav_frames * 4;
        uint8_t h[44] = { 'R','I','F','F', 0,0,0,0, 'W','A','V','E', 'f','m','t',' ', 16,0,0,0, 1,0, 2,0,
                          0,0,0,0, 0,0,0,0, 4,0, 16,0, 'd','a','t','a', 0,0,0,0 };
        uint32_t riff = 36 + data, bytes = rate * 4;
        memcpy(h + 4, &riff, 4); memcpy(h + 24, &rate, 4); memcpy(h + 28, &bytes, 4); memcpy(h + 40, &data, 4);
        fseek(wav, 0, SEEK_SET);
        fwrite(h, 1, 44, wav);
        fclose(wav);
    }
    if (save_eeprom_path && nsl_machine_save_eeprom(save_eeprom_path) != 0) {
        fprintf(stderr, "cannot write EEPROM image %s\n", save_eeprom_path);
    }
    if (dump_ram_path) nsl_write_file_atomic(dump_ram_path, nsl_machine.ram, sizeof(nsl_machine.ram));

    printf("frames=%d irqs=%llu cycles=%llu pc=%06X mode=%u\n", frames,
           (unsigned long long)nsl_cpu.irq_count, (unsigned long long)nsl_cpu.cycles, nsl_cpu.pc, nsl_cpu.mode);
    printf("ram=%08X fb=%08X pal=%08X\n", fnv1a(nsl_machine.ram, sizeof(nsl_machine.ram)),
           fnv1a(nsl_host.framebuffer, sizeof(nsl_host.framebuffer)),
           fnv1a(nsl_machine.palette_buffered, sizeof(nsl_machine.palette_buffered)));
    printf("vblank_acks=%u palette_dmas=%u sprite_dmas=%u/%u unmapped=%u/%u soundlatch=%u\n",
           nsl_machine.vblank_ack_count, nsl_machine.palette_dma_count,
           nsl_machine.sprite_dma_count[0], nsl_machine.sprite_dma_count[1],
           nsl_machine.unmapped_reads, nsl_machine.unmapped_writes, nsl_machine.soundlatch_writes);
    const uint32_t *missed;
    unsigned missed_count = nsl_interp_missed(&missed);
    if (missed_count) {
        printf("fallback: %u entry points, %llu instructions (add to tools/traced_targets.txt):\n",
               missed_count, (unsigned long long)nsl_interp_instruction_count());
        for (unsigned i = 0; i < missed_count; i++) printf("%06X\n", missed[i]);
    }
    nsl_core_unload();
    return rc;
}
