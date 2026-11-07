/*
 * Night Slashers headless runner: deterministic boot/frame harness.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "nsl/cpu.h"
#include "nsl/machine.h"
#include "nsl/run.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t *read_file(const char *path, size_t *size) {
    FILE *file = fopen(path, "rb");
    if (!file) {
        fprintf(stderr, "cannot open %s\n", path);
        return NULL;
    }
    fseek(file, 0, SEEK_END);
    long length = ftell(file);
    fseek(file, 0, SEEK_SET);
    if (length <= 0) {
        fclose(file);
        fprintf(stderr, "%s is empty\n", path);
        return NULL;
    }
    uint8_t *data = (uint8_t *)malloc((size_t)length);
    if (!data || fread(data, 1, (size_t)length, file) != (size_t)length) {
        fprintf(stderr, "cannot read %s\n", path);
        free(data);
        fclose(file);
        return NULL;
    }
    fclose(file);
    *size = (size_t)length;
    return data;
}

static uint32_t fnv1a(const void *data, size_t size) {
    const uint8_t *bytes = (const uint8_t *)data;
    uint32_t hash = 2166136261u;
    for (size_t index = 0; index < size; index++) {
        hash ^= bytes[index];
        hash *= 16777619u;
    }
    return hash;
}

int main(int argc, char **argv) {
    int frames = 600;
    const char *program_path = "build/assets/program.bin";
    const char *hash_path = NULL;
    const char *eeprom_path = NULL;
    const char *save_eeprom_path = NULL;
    const char *dump_ram_path = NULL;
    int trace = 0;
    int trace_cpu = 0;
    int trace_after = 0;
    int trace_deco = 0;
    int trace_eeprom = 0;
    uint32_t trace_pc = 0;
    int trace_pc_count = 0;

    for (int index = 1; index < argc; index++) {
        if (!strcmp(argv[index], "--frames") && index + 1 < argc) {
            frames = atoi(argv[++index]);
        } else if (!strcmp(argv[index], "--program") && index + 1 < argc) {
            program_path = argv[++index];
        } else if (!strcmp(argv[index], "--hashes") && index + 1 < argc) {
            hash_path = argv[++index];
        } else if (!strcmp(argv[index], "--eeprom") && index + 1 < argc) {
            eeprom_path = argv[++index];
        } else if (!strcmp(argv[index], "--save-eeprom") && index + 1 < argc) {
            save_eeprom_path = argv[++index];
        } else if (!strcmp(argv[index], "--dump-ram") && index + 1 < argc) {
            dump_ram_path = argv[++index];
        } else if (!strcmp(argv[index], "--trace")) {
            trace = 1;
        } else if (!strcmp(argv[index], "--trace-cpu") && index + 1 < argc) {
            trace_cpu = atoi(argv[++index]);
        } else if (!strcmp(argv[index], "--trace-irq")) {
            trace_cpu = -1;
        } else if (!strcmp(argv[index], "--trace-deco")) {
            trace_deco = 1;
        } else if (!strcmp(argv[index], "--trace-eeprom")) {
            trace_eeprom = 1;
        } else if (!strcmp(argv[index], "--trace-pc") && index + 2 < argc) {
            trace_pc = (uint32_t)strtoul(argv[++index], NULL, 0);
            trace_pc_count = atoi(argv[++index]);
        } else if (!strcmp(argv[index], "--trace-after") && index + 1 < argc) {
            trace_after = atoi(argv[++index]);
        } else {
            fprintf(stderr, "usage: %s [--frames N] [--program FILE] [--hashes FILE] [--trace]\n",
                    argv[0]);
            return 2;
        }
    }

    size_t program_size = 0;
    uint8_t *program = read_file(program_path, &program_size);
    if (!program) {
        return 2;
    }
    if (program_size != NSL_PROGRAM_SIZE) {
        fprintf(stderr, "%s: expected %u bytes, got %zu\n", program_path,
                NSL_PROGRAM_SIZE, program_size);
        free(program);
        return 2;
    }

    nsl_machine_init(program);
    if (eeprom_path && nsl_machine_load_eeprom(eeprom_path) != 0) {
        fprintf(stderr, "cannot load EEPROM image %s\n", eeprom_path);
        free(program);
        return 2;
    }
    nsl_cpu_reset();
    nsl_machine.trace_bus = trace;
    nsl_machine.trace_deco = trace_deco;
    nsl_machine.trace_eeprom = trace_eeprom;
    nsl_cpu.trace_blocks = trace_cpu > 0 ? trace_cpu : 0;
    nsl_cpu.trace_after = trace_after;
    nsl_cpu.trace_irq = trace_cpu < 0;
    nsl_cpu.trace_pc = trace_pc;
    nsl_cpu.trace_pc_count = trace_pc_count;

    FILE *hashes = hash_path ? fopen(hash_path, "w") : NULL;
    if (hash_path && !hashes) {
        fprintf(stderr, "cannot write %s\n", hash_path);
        free(program);
        return 2;
    }

    for (int frame = 0; frame < frames; frame++) {
        if (nsl_run_frame() != 0) {
            fprintf(stderr, "frame %d: %s\n", frame, nsl_cpu.fault_text);
            if (hashes) fclose(hashes);
            free(program);
            return 1;
        }
        if (hashes) {
            fprintf(hashes, "%d %08X %08X %08X %08X %08X %llu\n", frame,
                    fnv1a(nsl_machine.ram, sizeof(nsl_machine.ram)),
                    fnv1a(nsl_machine.palette, sizeof(nsl_machine.palette)),
                    fnv1a(nsl_machine.spriteram, sizeof(nsl_machine.spriteram)),
                    fnv1a(nsl_machine.tilevram, sizeof(nsl_machine.tilevram)),
                    fnv1a(nsl_machine.framebuffer, sizeof(nsl_machine.framebuffer)),
                    (unsigned long long)nsl_cpu.cycles);
        }
    }
    if (hashes) fclose(hashes);
    if (save_eeprom_path && nsl_machine_save_eeprom(save_eeprom_path) != 0) {
        fprintf(stderr, "cannot write EEPROM image %s\n", save_eeprom_path);
    }
    if (dump_ram_path) {
        FILE *dump = fopen(dump_ram_path, "wb");
        if (dump) {
            fwrite(nsl_machine.ram, 1, sizeof(nsl_machine.ram), dump);
            fclose(dump);
        }
    }

    printf("frames=%d irqs=%llu cycles=%llu pc=%06X mode=%u\n", frames,
           (unsigned long long)nsl_cpu.irq_count,
           (unsigned long long)nsl_cpu.cycles, nsl_cpu.pc, nsl_cpu.mode);
    printf("ram=%08X pal=%08X spr=%08X vram=%08X fb=%08X\n",
           fnv1a(nsl_machine.ram, sizeof(nsl_machine.ram)),
           fnv1a(nsl_machine.palette, sizeof(nsl_machine.palette)),
           fnv1a(nsl_machine.spriteram, sizeof(nsl_machine.spriteram)),
           fnv1a(nsl_machine.tilevram, sizeof(nsl_machine.tilevram)),
           fnv1a(nsl_machine.framebuffer, sizeof(nsl_machine.framebuffer)));
    printf("vblank_acks=%u palette_dmas=%u sprite_dmas=%u/%u unmapped=%u/%u\n",
           nsl_machine.vblank_ack_count, nsl_machine.palette_dma_count,
           nsl_machine.sprite_dma_count[0], nsl_machine.sprite_dma_count[1],
           nsl_machine.unmapped_reads, nsl_machine.unmapped_writes);
    printf("history:");
    for (int index = 0; index < 32; index++) {
        unsigned at = (nsl_cpu.history_index - 32 + index) & 31;
        printf(" %06X", nsl_cpu.history[at]);
    }
    printf("\n");
    free(program);
    return nsl_cpu.fault ? 1 : 0;
}
