/*
 * Night Slashers native runtime: save states.
 *
 * A state is the complete board at a frame boundary (the only point where the
 * front end can call in): translated-CPU registers and banks, work RAM, tile chips,
 * rowscroll, palette/ACE, sprite RAM and buffers, DECO104 protection chip, EEPROM
 * device (including its serial state machine), interrupt/VBLANK timing and the
 * sound section. The frame that was on screen is stored too, so a loaded state
 * shows the right picture before the next frame is emulated.
 *
 * Layout: NslStateHeader, then a zlib-compressed payload of tagged sections.
 * Everything is validated (magic, version, game, program CRC, ROM fingerprint,
 * structure sizes, CRCs, section tags and sizes) before any live state changes.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "nsl/state.h"
#include "nsl/core.h"
#include "nsl/cpu.h"
#include "nsl/machine.h"
#include "nsl/sound.h"
#include "miniz.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct Section {
    char tag[4];
    uint32_t size;
} Section;

typedef struct CpuSnapshot {
    NslCpu cpu;
} CpuSnapshot;

static size_t payload_size(void) {
    return sizeof(Section) + sizeof(NslCpu)
         + sizeof(Section) + sizeof(NslMachine)
         + sizeof(Section) + sizeof(NslEepromState)
         + sizeof(Section) + nsl_sound_state_size()
         + sizeof(Section) + sizeof(nsl_host.framebuffer);
}

size_t nsl_core_state_size(void) {
    return sizeof(NslStateHeader) + mz_compressBound((mz_ulong)payload_size());
}

static uint8_t *put_section(uint8_t *p, const char *tag, const void *data, uint32_t size) {
    Section s;
    memcpy(s.tag, tag, 4);
    s.size = size;
    memcpy(p, &s, sizeof(s));
    p += sizeof(s);
    if (size) memcpy(p, data, size);
    return p + size;
}

static uint32_t crc32_of(const void *data, size_t size) {
    return (uint32_t)mz_crc32(MZ_CRC32_INIT, (const unsigned char *)data, size);
}

static uint32_t header_crc(const NslStateHeader *header) {
    NslStateHeader copy = *header;
    copy.header_crc = 0;
    return crc32_of(&copy, sizeof(copy));
}

int nsl_core_save_state(uint8_t *buffer, size_t size) {
    if (!nsl_core_loaded() || nsl_cpu.fault) return -1;
    size_t raw_size = payload_size();
    uint8_t *raw = (uint8_t *)malloc(raw_size);
    if (!raw) return -1;
    uint8_t *p = raw;
    NslCpu cpu = nsl_cpu;
    cpu.trace_blocks = cpu.trace_after = cpu.trace_pc_count = cpu.trace_irq = 0;
    cpu.trace_pc = 0;
    p = put_section(p, "CPU ", &cpu, sizeof(cpu));
    p = put_section(p, "MACH", &nsl_machine, sizeof(nsl_machine));
    NslEepromState eeprom;
    nsl_eeprom_get_state(&eeprom);
    p = put_section(p, "EEPR", &eeprom, sizeof(eeprom));
    uint32_t sound_size = (uint32_t)nsl_sound_state_size();
    Section s;
    memcpy(s.tag, "SND ", 4);
    s.size = sound_size;
    memcpy(p, &s, sizeof(s));
    p += sizeof(s);
    nsl_sound_save(p);
    p += sound_size;
    p = put_section(p, "FRAM", nsl_host.framebuffer, sizeof(nsl_host.framebuffer));

    NslStateHeader header;
    memset(&header, 0, sizeof(header));
    memcpy(header.magic, NSL_STATE_MAGIC, 8);
    header.version = NSL_STATE_VERSION;
    header.header_size = sizeof(header);
    snprintf(header.game, sizeof(header.game), "nslasher");
    header.program_crc = nsl_translated_program_crc32;
    header.rom_fingerprint = nsl_core_rom_fingerprint();
    header.layout_cpu = sizeof(NslCpu);
    header.layout_machine = sizeof(NslMachine);
    header.layout_sound = sound_size;
    header.payload_size = (uint32_t)raw_size;
    header.payload_crc = crc32_of(raw, raw_size);
    header.frame_number = nsl_machine.frame_count;

    mz_ulong compressed = (mz_ulong)(size - sizeof(header));
    int z = mz_compress2(buffer + sizeof(header), &compressed, raw, (mz_ulong)raw_size, 6);
    free(raw);
    if (z != MZ_OK) return -1;
    header.compressed_size = (uint32_t)compressed;
    header.header_crc = header_crc(&header);
    memcpy(buffer, &header, sizeof(header));
    return (int)(sizeof(header) + compressed);
}

void nsl_state_set_timestamp(uint8_t *buffer, size_t size, int64_t timestamp) {
    if (size < sizeof(NslStateHeader)) return;
    NslStateHeader header;
    memcpy(&header, buffer, sizeof(header));
    header.timestamp = timestamp;
    header.header_crc = header_crc(&header);
    memcpy(buffer, &header, sizeof(header));
}

int nsl_state_peek(const uint8_t *buffer, size_t size, NslStateHeader *header, char *error, size_t error_size) {
    if (size < sizeof(NslStateHeader)) {
        snprintf(error, error_size, "The file is too short to be a save state.");
        return -1;
    }
    memcpy(header, buffer, sizeof(*header));
    if (memcmp(header->magic, NSL_STATE_MAGIC, 8) != 0) {
        snprintf(error, error_size, "Not a Night Slashers save state.");
        return -1;
    }
    if (header->header_size != sizeof(*header) || header_crc(header) != header->header_crc) {
        snprintf(error, error_size, "The save state header is damaged.");
        return -1;
    }
    if (header->version != NSL_STATE_VERSION) {
        snprintf(error, error_size, "Save state format %u is not supported (this build reads format %u).",
                 header->version, NSL_STATE_VERSION);
        return -1;
    }
    if (strncmp(header->game, "nslasher", sizeof(header->game)) != 0) {
        snprintf(error, error_size, "This save state belongs to another game (%.16s).", header->game);
        return -1;
    }
    if ((size_t)header->compressed_size + sizeof(*header) > size) {
        snprintf(error, error_size, "The save state is truncated.");
        return -1;
    }
    return 0;
}

static const uint8_t *take(const uint8_t **p, const uint8_t *end, const char *tag, uint32_t expected) {
    if ((size_t)(end - *p) < sizeof(Section)) return NULL;
    Section s;
    memcpy(&s, *p, sizeof(s));
    if (memcmp(s.tag, tag, 4) != 0 || s.size != expected || (size_t)(end - *p) < sizeof(s) + s.size) return NULL;
    const uint8_t *data = *p + sizeof(s);
    *p = data + s.size;
    return data;
}

int nsl_core_load_state(const uint8_t *buffer, size_t size, char *error, size_t error_size) {
    NslStateHeader header;
    if (!nsl_core_loaded()) {
        snprintf(error, error_size, "Load a ROM before loading a state.");
        return -1;
    }
    if (nsl_state_peek(buffer, size, &header, error, error_size) != 0) return -1;
    if (header.program_crc != nsl_translated_program_crc32) {
        snprintf(error, error_size, "This state was made with a different program revision (%08X).", header.program_crc);
        return -1;
    }
    if (header.rom_fingerprint != nsl_core_rom_fingerprint()) {
        snprintf(error, error_size, "This state was made with different ROM data.");
        return -1;
    }
    if (header.layout_cpu != sizeof(NslCpu) || header.layout_machine != sizeof(NslMachine)
        || header.layout_sound != nsl_sound_state_size()) {
        snprintf(error, error_size, "This state was made by an incompatible build of the runtime.");
        return -1;
    }
    if (header.payload_size != payload_size()) {
        snprintf(error, error_size, "The save state layout does not match this build.");
        return -1;
    }
    uint8_t *raw = (uint8_t *)malloc(header.payload_size);
    if (!raw) { snprintf(error, error_size, "Out of memory."); return -1; }
    mz_ulong raw_size = header.payload_size;
    if (mz_uncompress(raw, &raw_size, buffer + sizeof(header), header.compressed_size) != MZ_OK
        || raw_size != header.payload_size || crc32_of(raw, raw_size) != header.payload_crc) {
        free(raw);
        snprintf(error, error_size, "The save state is damaged (checksum mismatch).");
        return -1;
    }
    const uint8_t *p = raw, *end = raw + raw_size;
    const uint8_t *cpu = take(&p, end, "CPU ", sizeof(NslCpu));
    const uint8_t *machine = cpu ? take(&p, end, "MACH", sizeof(NslMachine)) : NULL;
    const uint8_t *eeprom = machine ? take(&p, end, "EEPR", sizeof(NslEepromState)) : NULL;
    const uint8_t *sound = eeprom ? take(&p, end, "SND ", (uint32_t)nsl_sound_state_size()) : NULL;
    const uint8_t *frame = sound ? take(&p, end, "FRAM", sizeof(nsl_host.framebuffer)) : NULL;
    if (!frame || p != end) {
        free(raw);
        snprintf(error, error_size, "The save state sections are damaged.");
        return -1;
    }
    NslCpu restored;
    memcpy(&restored, cpu, sizeof(restored));
    if (restored.fault || restored.mode > 3) {
        free(raw);
        snprintf(error, error_size, "The save state holds an invalid CPU state.");
        return -1;
    }

    /* validated: apply */
    NslCpu keep = nsl_cpu;
    nsl_cpu = restored;
    nsl_cpu.trace_blocks = keep.trace_blocks;
    nsl_cpu.trace_after = keep.trace_after;
    nsl_cpu.trace_pc = keep.trace_pc;
    nsl_cpu.trace_pc_count = keep.trace_pc_count;
    nsl_cpu.trace_irq = keep.trace_irq;
    memcpy(&nsl_machine, machine, sizeof(nsl_machine));
    NslEepromState eeprom_state;
    memcpy(&eeprom_state, eeprom, sizeof(eeprom_state));
    nsl_eeprom_set_state(&eeprom_state);
    nsl_sound_load(sound);
    memcpy(nsl_host.framebuffer, frame, sizeof(nsl_host.framebuffer));
    nsl_machine.rendered_until = NSL_SCREEN_TOP;
    free(raw);
    return 0;
}
