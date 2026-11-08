/*
 * Night Slashers native runtime: bus, DECO ACE palette, sprite/tile RAM, EEPROM, inputs.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "nsl/machine.h"
#include "nsl/cpu.h"
#include "nsl/platform.h"
#include "nsl/sound.h"
#include "deco104_table.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

NslMachine nsl_machine;
NslMachineHost nsl_host;

/* 93C46 (16-bit organisation), modelled on MAME's eeprom_serial_93cxx_device
 * (src/devices/machine/eepromser.cpp, eeprom.cpp). */
static int eeprom_cs, eeprom_clk, eeprom_di;
enum { EE_RESET, EE_START, EE_COMMAND, EE_READ, EE_WRITE, EE_COMPLETE };
static int eeprom_state;
static int eeprom_bits;
static unsigned eeprom_accum;
static unsigned eeprom_address;
static uint32_t eeprom_shift;
static uint16_t eeprom_data[64];
static int eeprom_locked;
static uint64_t eeprom_ready_cycle;   /* CPU cycle at which the write cycle ends */

/* MAME eeprom.cpp operation times at the 7 MHz ARM clock. */
#define EE_WRITE_CYCLES     (7000000ull * 1750 / 1000000)
#define EE_ERASE_CYCLES     (7000000ull * 1000 / 1000000)
#define EE_ALL_CYCLES       (7000000ull * 8000 / 1000000)

static int eeprom_ready(void) {
    return nsl_cpu.cycles >= eeprom_ready_cycle;
}

static int eeprom_do_read(void) {
    if (eeprom_state == EE_READ) {
        return (eeprom_shift >> 31) & 1;
    }
    if (eeprom_state == EE_START) {
        return eeprom_ready();
    }
    return 1;
}

void nsl_machine_set_default_inputs(void) {
    nsl_machine_set_inputs(0xFFFF, 0xFFFF);
}

void nsl_machine_set_inputs(uint16_t in0, uint16_t in1) {
    nsl_machine.inputs_in0 = in0;
    nsl_machine.inputs_in1 = (uint16_t)((in1 & ~0x0010u) | (nsl_machine.vblank_active ? 0x0010u : 0u));
}

void nsl_machine_set_vblank(int active) {
    nsl_machine.vblank_active = active != 0;
    nsl_machine.inputs_in1 = (uint16_t)((nsl_machine.inputs_in1 & ~0x0010u)
                                        | (nsl_machine.vblank_active ? 0x0010u : 0u));
}

/* Power-on/reset state of the board (EEPROM contents are kept, like the real chip). */
void nsl_machine_reset(void) {
    uint16_t in0 = nsl_machine.inputs_in0, in1 = nsl_machine.inputs_in1;
    memset(&nsl_machine, 0, sizeof(nsl_machine));
    nsl_machine.volume = 0;              /* 0x00 = full volume (volume_w) */
    for (unsigned bank = 0; bank < 2; bank++) {
        for (unsigned index = 0; index < 0x80; index++) {
            nsl_machine.deco_ram[bank][index] = 0xFFFF;
        }
    }
    nsl_machine.deco_latchaddr = 0xFFFF;
    /* fghthist.cpp nslasher(): set_col_bank<0>/<1> for each tile generator */
    nsl_machine.tilegen[0].colour_bank[0] = 0x00;
    nsl_machine.tilegen[0].colour_bank[1] = 0x10;
    nsl_machine.tilegen[1].colour_bank[0] = 0x20;
    nsl_machine.tilegen[1].colour_bank[1] = 0x30;
    nsl_machine.rendered_until = NSL_SCREEN_TOP;
    eeprom_state = EE_RESET;
    eeprom_cs = eeprom_clk = eeprom_di = 0;
    eeprom_locked = 1;
    eeprom_ready_cycle = 0;
    nsl_machine.inputs_in0 = in0 ? in0 : 0xFFFF;
    nsl_machine.inputs_in1 = in1 ? in1 : 0xFFFF;
}

void nsl_machine_init(const uint8_t *decrypted_program) {
    nsl_host.program = decrypted_program;
    for (unsigned index = 0; index < 64; index++) {
        eeprom_data[index] = 0xFFFF;
    }
    nsl_machine.inputs_in0 = nsl_machine.inputs_in1 = 0xFFFF;
    nsl_machine_reset();
}

void nsl_machine_get_eeprom(uint8_t out[128]) {
    for (unsigned index = 0; index < 64; index++) {      /* MAME .nv layout: u16 LE */
        out[index * 2] = (uint8_t)(eeprom_data[index] & 0xFF);
        out[index * 2 + 1] = (uint8_t)(eeprom_data[index] >> 8);
    }
}

void nsl_machine_set_eeprom(const uint8_t in[128]) {
    for (unsigned index = 0; index < 64; index++) {
        eeprom_data[index] = (uint16_t)(in[index * 2] | (in[index * 2 + 1] << 8));
    }
}

int nsl_machine_load_eeprom(const char *path) {
    size_t size = 0;
    uint8_t *raw = nsl_read_file(path, &size);
    if (!raw) return -1;
    if (size != 128) { free(raw); return -1; }
    nsl_machine_set_eeprom(raw);
    free(raw);
    return 0;
}

int nsl_machine_save_eeprom(const char *path) {
    uint8_t raw[128];
    nsl_machine_get_eeprom(raw);
    return nsl_write_file_atomic(path, raw, sizeof(raw));
}

void nsl_eeprom_get_state(NslEepromState *s) {
    memset(s, 0, sizeof(*s));
    s->cs = eeprom_cs; s->clk = eeprom_clk; s->di = eeprom_di;
    s->state = eeprom_state; s->bits = eeprom_bits; s->locked = eeprom_locked;
    s->accum = eeprom_accum; s->address = eeprom_address; s->shift = eeprom_shift;
    s->ready_cycle = eeprom_ready_cycle;
    memcpy(s->data, eeprom_data, sizeof(eeprom_data));
}

void nsl_eeprom_set_state(const NslEepromState *s) {
    eeprom_cs = s->cs; eeprom_clk = s->clk; eeprom_di = s->di;
    eeprom_state = s->state; eeprom_bits = s->bits; eeprom_locked = s->locked;
    eeprom_accum = s->accum; eeprom_address = s->address & 0x7F; eeprom_shift = s->shift;
    eeprom_ready_cycle = s->ready_cycle;
    memcpy(eeprom_data, s->data, sizeof(eeprom_data));
}

/* ---- 93C46 serial EEPROM (MAME's eeprom_serial_base_device::handle_event) -- */

static void eeprom_clock_rising(void) {
    switch (eeprom_state) {
    case EE_START:
        if (eeprom_di && eeprom_ready()) {
            eeprom_state = EE_COMMAND;
            eeprom_accum = 0;
            eeprom_bits = 0;
        }
        break;
    case EE_COMMAND:
        eeprom_accum = (eeprom_accum << 1) | (unsigned)eeprom_di;
        if (++eeprom_bits == 8) {
            unsigned command = (eeprom_accum >> 6) & 3;
            eeprom_address = eeprom_accum & 0x3F;
            eeprom_bits = 0;
            if (nsl_host.trace_eeprom) {
                fprintf(stderr, "eeprom cmd %u addr %02X\n", command, eeprom_address);
            }
            switch (command) {
            case 2:                                     /* READ: dummy 0 first */
                eeprom_shift = 0;
                eeprom_state = EE_READ;
                break;
            case 1:                                     /* WRITE */
                eeprom_shift = 0;
                eeprom_state = EE_WRITE;
                break;
            case 3:                                     /* ERASE */
                if (eeprom_locked) { eeprom_state = EE_RESET; break; }
                eeprom_data[eeprom_address] = 0xFFFF;
                eeprom_ready_cycle = nsl_cpu.cycles + EE_ERASE_CYCLES;
                eeprom_state = EE_COMPLETE;
                break;
            default:
                switch (eeprom_address >> 4) {
                case 0: eeprom_locked = 1; eeprom_state = EE_RESET; break;     /* EWDS */
                case 1: eeprom_state = EE_WRITE; eeprom_shift = 0;              /* WRAL */
                        eeprom_address = 0x40; break;
                case 2:                                                         /* ERAL */
                    if (eeprom_locked) { eeprom_state = EE_RESET; break; }
                    for (unsigned i = 0; i < 64; i++) eeprom_data[i] = 0xFFFF;
                    eeprom_ready_cycle = nsl_cpu.cycles + EE_ALL_CYCLES;
                    eeprom_state = EE_COMPLETE;
                    break;
                default: eeprom_locked = 0; eeprom_state = EE_RESET; break;    /* EWEN */
                }
                break;
            }
        }
        break;
    case EE_READ: {
        int bit_index = eeprom_bits++;
        if (bit_index == 0) {
            eeprom_shift = (uint32_t)eeprom_data[eeprom_address] << 16;
            if (nsl_host.trace_eeprom) {
                fprintf(stderr, "eeprom read %02X = %04X\n", eeprom_address,
                        eeprom_data[eeprom_address]);
            }
        } else {
            eeprom_shift = (eeprom_shift << 1) | 1;
        }
        break;
    }
    case EE_WRITE:
        eeprom_shift = (eeprom_shift << 1) | (unsigned)eeprom_di;
        if (++eeprom_bits == 16) {
            if (eeprom_locked) {
                eeprom_state = EE_RESET;
                break;
            }
            if (eeprom_address == 0x40) {
                for (unsigned i = 0; i < 64; i++) eeprom_data[i] &= (uint16_t)eeprom_shift;
                eeprom_ready_cycle = nsl_cpu.cycles + EE_ALL_CYCLES;
            } else {
                eeprom_data[eeprom_address] = (uint16_t)eeprom_shift;
                eeprom_ready_cycle = nsl_cpu.cycles + EE_WRITE_CYCLES;
            }
            eeprom_state = EE_COMPLETE;
            if (nsl_host.trace_eeprom) {
                fprintf(stderr, "eeprom write %02X = %04X\n", eeprom_address,
                        (unsigned)(eeprom_shift & 0xFFFF));
            }
        }
        break;
    default:
        break;
    }
}

/* MAME fghthist_common_state::eeprom_w: clk_write, di_write, then cs_write. */
static void eeprom_write(uint8_t value) {
    int cs = (value >> 6) & 1;
    int clk = (value >> 5) & 1;
    int di = (value >> 4) & 1;
    if (clk != eeprom_clk) {
        eeprom_clk = clk;
        if (clk && eeprom_state != EE_RESET) {
            eeprom_clock_rising();
        }
    }
    eeprom_di = di;
    if (cs != eeprom_cs) {
        eeprom_cs = cs;
        if (cs) {
            if (eeprom_state == EE_RESET) {
                eeprom_state = EE_START;
            }
        } else {
            eeprom_state = EE_RESET;
        }
        eeprom_bits = 0;
    }
}

/* ---- DECO104 protection / I/O ------------------------------------------ */

static uint16_t bitswap16(uint16_t value, const unsigned char *positions) {
    uint16_t result = 0;
    for (int index = 0; index < 16; index++) {
        result = (uint16_t)((result << 1) | ((value >> positions[index]) & 1));
    }
    return result;
}

static uint16_t deco_reorder(uint16_t input, const unsigned char *weights) {
    uint16_t result = 0;
    for (int bit = 0; bit < 16; bit++) {
        if ((input >> bit) & 1) {
            if (weights[bit] != 0xFF) {
                result |= (uint16_t)(1u << weights[bit]);
            }
        }
    }
    return result;
}

static uint16_t deco_read_protport(uint16_t address) {
    if (address == nsl_machine.deco_latchaddr && nsl_machine.deco_latchflag) {
        nsl_machine.deco_latchflag = 0;
        return nsl_machine.deco_latchdata;
    }
    nsl_machine.deco_latchflag = 0;

    unsigned index = (address >> 1) & 0x3FF;
    const NslDeco104Entry *entry = &nsl_deco104_table[index];
    int location = entry->offset;
    uint16_t data;
    if (location == -1) {
        data = nsl_machine.inputs_in0;
    } else if (location == -2) {
        data = (uint16_t)eeprom_do_read();
    } else if (location == -3) {
        data = nsl_machine.inputs_in1;
    } else {
        data = nsl_machine.deco_ram[nsl_machine.deco_bank][(location & 0xFF) >> 1];
    }
    uint16_t result = deco_reorder(data, entry->mapping);
    if (entry->use_xor) {
        result ^= nsl_machine.deco_xor;
    }
    if (entry->use_nand) {
        result &= (uint16_t)~nsl_machine.deco_nand;
    }
    if (nsl_host.trace_deco) {
        fprintf(stderr, "deco read addr=%03X index=%03X loc=%d data=%04X -> %04X (in0=%04X in1=%04X)\n",
                address, index, location, data, result,
                nsl_machine.inputs_in0, nsl_machine.inputs_in1);
    }
    if (location == 0x66) {           /* bankswitch port */
        nsl_machine.deco_bank ^= 1;
    }
    return result;
}

static void deco_write_protport(uint16_t address, uint16_t data) {
    nsl_machine.deco_latchaddr = address;
    nsl_machine.deco_latchdata = data;
    nsl_machine.deco_latchflag = 1;
    unsigned low = address & 0xFF;
    if (low == 0x42) {
        nsl_machine.deco_xor = data;
    } else if (low == 0xEE) {
        nsl_machine.deco_nand = data;
    } else if (low == 0xA8) {
        nsl_machine.soundlatch = (uint16_t)(data & 0xFF);
        nsl_machine.soundlatch_writes++;
        nsl_sound_latch_write((uint8_t)data);   /* soundlatch_irq_cb -> Z80 IRQ */
    }
    nsl_machine.deco_ram[nsl_machine.deco_bank][low >> 1] = data;
}

static uint16_t deco_read_data(uint16_t address) {
    static const unsigned char scramble[16] = {
        15, 14, 13, 12, 11, 10,
        4, 5, 3, 6, 2, 7, 1, 8, 0, 9
    };
    address = (uint16_t)(bitswap16((uint16_t)(address >> 1), scramble) << 1);
    unsigned upper = (address & 0x7800) >> 11;
    if (upper == 0x8) {
        return 0;
    }
    for (int index = 0; index < 6; index++) {
        if (nsl_machine.deco_region[index] == upper && index == 0) {
            return deco_read_protport(address & 0x7FF);
        }
    }
    return 0;
}

static void deco_write_data(uint16_t address, uint16_t data) {
    static const unsigned char scramble[16] = {
        15, 14, 13, 12, 11, 10,
        4, 5, 3, 6, 2, 7, 1, 8, 0, 9
    };
    address = (uint16_t)(bitswap16((uint16_t)(address >> 1), scramble) << 1);
    unsigned upper = (address & 0x7800) >> 11;
    if (upper == 0x8) {
        unsigned reg = address & 0xF;
        if (reg >= 2 && reg <= 0xC) {
            nsl_machine.deco_region[(reg - 2) / 2] = (uint8_t)(data & 0xF);
        }
        return;
    }
    for (int index = 0; index < 6; index++) {
        if (nsl_machine.deco_region[index] == upper && index == 0) {
            deco_write_protport(address & 0x7FF, data);
        }
    }
}

static uint16_t deco104_read(uint32_t address) {
    uint32_t real = ((address - 0x200000) & 0x7FFF) >> 1;
    uint16_t deco_address = (uint16_t)((((real >> 14) & 0xF) << 11) | (real & 0x7FF));
    return deco_read_data(deco_address);
}

static void deco104_write(uint32_t address, uint16_t data) {
    uint32_t real = ((address - 0x200000) & 0x7FFF) >> 1;
    uint16_t deco_address = (uint16_t)((((real >> 14) & 0xF) << 11) | (real & 0x7FF));
    deco_write_data(deco_address, data);
}

/* ---- 32-bit board bus ---------------------------------------------------
 *
 * Every access is reduced to (dword address, data, byte-lane mask), as MAME's
 * address map does for a 32-bit little-endian CPU. Devices then see exactly the
 * lanes they are wired to (MAME umask32).
 */

static inline uint16_t combine16(uint16_t old, uint32_t data, uint32_t mask) {
    return (uint16_t)((old & ~mask) | (data & mask));
}

static void palette_dma(void) {
    memcpy(nsl_machine.palette_buffered, nsl_machine.palette, sizeof(nsl_machine.palette));
    nsl_machine.palette_dma_count++;
}

static void sprite_dma(unsigned which) {
    memcpy(nsl_machine.sprite_buffered[which], nsl_machine.spriteram[which],
           sizeof(nsl_machine.spriteram[which]));
    nsl_machine.sprite_dma_count[which]++;
}

void nsl_vblank_ack(void) {
    /* MAME vblank_ack_w: clears the ARM IRQ line (asserted by irq0_line_assert). */
    nsl_cpu.pending_irq = 0;
    nsl_machine.vblank_ack_count++;
}

void nsl_palette_dma(uint32_t source) { (void)source; palette_dma(); }
void nsl_sprite_dma(unsigned which, uint32_t source) { (void)source; if (which < 2) sprite_dma(which); }
void nsl_ace_write(unsigned index, uint32_t value) { if (index < 0x28) nsl_machine.ace[index] = (uint16_t)value; }
uint32_t nsl_ace_read(unsigned index) { return index < 0x28 ? nsl_machine.ace[index] : 0; }

static uint32_t bus_read_dword(uint32_t address) {
    if (address < NSL_PROGRAM_SIZE) {
        const uint8_t *p = nsl_host.program + address;
        return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    }
    if (address >= 0x100000 && address < 0x120000) {
        const uint8_t *p = nsl_machine.ram + (address - 0x100000);
        return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    }
    if (address >= 0x163000 && address < 0x1630A0) {          /* ACE, umask 0x0000ffff */
        return nsl_machine.ace[(address - 0x163000) >> 2];
    }
    if (address >= 0x168000 && address < 0x16A000) {
        return nsl_machine.palette[(address - 0x168000) >> 2];
    }
    if (address >= 0x170000 && address < 0x172000) {
        return nsl_machine.spriteram[0][(address - 0x170000) >> 2] ^ 0xFFFF0000u;
    }
    if (address >= 0x178000 && address < 0x17A000) {
        return nsl_machine.spriteram[1][(address - 0x178000) >> 2] ^ 0xFFFF0000u;
    }
    if (address >= 0x182000 && address < 0x186000) {
        unsigned which = (address - 0x182000) >> 13;
        return nsl_machine.tilegen[0].vram[which][((address - 0x182000) & 0x1FFF) >> 2] | 0xFFFF0000u;
    }
    if (address >= 0x192000 && address < 0x196000) {
        unsigned which = (address - 0x192000) >> 13;
        return nsl_machine.rowscroll32[which][((address - 0x192000) & 0x1FFF) >> 2];
    }
    if (address >= 0x1A0000 && address < 0x1A0020) {
        return nsl_machine.tilegen[0].control[(address - 0x1A0000) >> 2] | 0xFFFF0000u;
    }
    if (address >= 0x1C2000 && address < 0x1C6000) {
        unsigned which = (address - 0x1C2000) >> 13;
        return nsl_machine.tilegen[1].vram[which][((address - 0x1C2000) & 0x1FFF) >> 2] | 0xFFFF0000u;
    }
    if (address >= 0x1D2000 && address < 0x1D6000) {
        unsigned which = (address - 0x1D2000) >> 13;
        return nsl_machine.rowscroll32[2 + which][((address - 0x1D2000) & 0x1FFF) >> 2];
    }
    if (address >= 0x1E0000 && address < 0x1E0020) {
        return nsl_machine.tilegen[1].control[(address - 0x1E0000) >> 2] | 0xFFFF0000u;
    }
    if (address >= 0x200000 && address < 0x208000) {
        /* ioprot_r on the upper half; nslasher_debug_r returns 0xffff on the lower half */
        return ((uint32_t)deco104_read(address) << 16) | 0xFFFFu;
    }
    nsl_machine.unmapped_reads++;
    return 0;
}

static void tilegen_control_write(unsigned chip, unsigned reg, uint32_t data, uint32_t mask) {
    mask &= 0xFFFF;
    if (!mask) return;
    nsl_video_partial_update();                 /* deco16ic control_w: update_partial */
    nsl_machine.tilegen[chip].control[reg] = combine16(nsl_machine.tilegen[chip].control[reg], data, mask);
}

static void bus_write_dword(uint32_t address, uint32_t data, uint32_t mask) {
    if (address >= 0x100000 && address < 0x120000) {
        uint8_t *p = nsl_machine.ram + (address - 0x100000);
        for (int lane = 0; lane < 4; lane++) {
            if (mask & (0xFFu << (lane * 8))) p[lane] = (uint8_t)(data >> (lane * 8));
        }
        return;
    }
    if (address == 0x140000) { nsl_vblank_ack(); return; }
    if (address == 0x150000) {
        if (mask & 0x000000FFu) {
            /* fghthist_common_state::eeprom_w: EEPROM lines and pri_w(data & 7) */
            eeprom_write((uint8_t)data);
            nsl_machine.pri = (uint8_t)(data & 7);
        }
        if (mask & 0x0000FF00u) nsl_machine.volume = (data >> 8) & 0xFF;
        return;
    }
    if (address >= 0x163000 && address < 0x1630A0) {
        unsigned index = (address - 0x163000) >> 2;
        nsl_machine.ace[index] = combine16(nsl_machine.ace[index], data, mask & 0xFFFF);
        return;
    }
    if (address == 0x164000) {
        if (mask & 0xFF) {
            /* tilemap_color_bank_w: second tile generator's colour banks */
            nsl_video_partial_update();
            nsl_machine.tilegen[1].colour_bank[0] = (uint16_t)(((data >> 0) & 7) << 4);
            nsl_machine.tilegen[1].colour_bank[1] = (uint16_t)(((data >> 3) & 7) << 4);
        }
        return;
    }
    if (address == 0x164004) { if (mask & 0xFF) nsl_machine.sprite_colourbase[0] = (uint16_t)((data & 7) << 8); return; }
    if (address == 0x164008) { if (mask & 0xFF) nsl_machine.sprite_colourbase[1] = (uint16_t)((data & 7) << 8); return; }
    if (address == 0x16400C || address == 0x16C000) return;
    if (address >= 0x168000 && address < 0x16A000) {
        unsigned index = (address - 0x168000) >> 2;
        nsl_machine.palette[index] = (nsl_machine.palette[index] & ~mask) | (data & mask);
        return;
    }
    if (address == 0x16C008) { palette_dma(); return; }
    if (address >= 0x170000 && address < 0x172000) {
        unsigned index = (address - 0x170000) >> 2;
        nsl_machine.spriteram[0][index] = combine16(nsl_machine.spriteram[0][index], data, mask & 0xFFFF);
        return;
    }
    if (address >= 0x178000 && address < 0x17A000) {
        unsigned index = (address - 0x178000) >> 2;
        nsl_machine.spriteram[1][index] = combine16(nsl_machine.spriteram[1][index], data, mask & 0xFFFF);
        return;
    }
    if (address == 0x174010) { sprite_dma(0); return; }
    if (address == 0x17C010) { sprite_dma(1); return; }
    if (address == 0x174000 || address == 0x174018 || address == 0x17C000 || address == 0x17C018) return;
    if (address >= 0x182000 && address < 0x186000) {
        unsigned which = (address - 0x182000) >> 13;
        uint16_t *cell = &nsl_machine.tilegen[0].vram[which][((address - 0x182000) & 0x1FFF) >> 2];
        *cell = combine16(*cell, data, mask & 0xFFFF);
        return;
    }
    if (address >= 0x192000 && address < 0x196000) {
        unsigned which = (address - 0x192000) >> 13;
        uint32_t *cell = &nsl_machine.rowscroll32[which][((address - 0x192000) & 0x1FFF) >> 2];
        *cell = (*cell & ~mask) | (data & mask);
        return;
    }
    if (address >= 0x1A0000 && address < 0x1A0020) {
        tilegen_control_write(0, (address - 0x1A0000) >> 2, data, mask);
        return;
    }
    if (address >= 0x1C2000 && address < 0x1C6000) {
        unsigned which = (address - 0x1C2000) >> 13;
        uint16_t *cell = &nsl_machine.tilegen[1].vram[which][((address - 0x1C2000) & 0x1FFF) >> 2];
        *cell = combine16(*cell, data, mask & 0xFFFF);
        return;
    }
    if (address >= 0x1D2000 && address < 0x1D6000) {
        unsigned which = (address - 0x1D2000) >> 13;
        uint32_t *cell = &nsl_machine.rowscroll32[2 + which][((address - 0x1D2000) & 0x1FFF) >> 2];
        *cell = (*cell & ~mask) | (data & mask);
        return;
    }
    if (address >= 0x1E0000 && address < 0x1E0020) {
        tilegen_control_write(1, (address - 0x1E0000) >> 2, data, mask);
        return;
    }
    if (address >= 0x200000 && address < 0x208000) {
        if (mask & 0xFFFF0000u) {           /* ioprot_w, umask32(0xffff0000) */
            deco104_write(address, (uint16_t)(data >> 16));
        }
        return;
    }
    if (address == 0x120000 || (address > 0x120000 && address < 0x120100)) return;   /* nopw */
    nsl_machine.unmapped_writes++;
}

static void trace_read(uint32_t address) {
    if (nsl_host.trace_bus && address != nsl_host.last_read_address) {
        fprintf(stderr, "read  %06X\n", address);
        nsl_host.last_read_address = address;
    }
}

static void trace_write(uint32_t address, uint32_t value) {
    if (nsl_host.trace_bus && address != nsl_host.last_write_address) {
        fprintf(stderr, "write %06X = %08X\n", address, value);
        nsl_host.last_write_address = address;
    }
}

uint32_t nsl_bus_read8(uint32_t address) {
    address &= NSL_ADDR26;
    trace_read(address);
    if (address >= 0x100000 && address < 0x120000) return nsl_machine.ram[address - 0x100000];
    if (address < NSL_PROGRAM_SIZE) return nsl_host.program[address];
    return (bus_read_dword(address & ~3u) >> ((address & 3) * 8)) & 0xFF;
}

uint32_t nsl_bus_read16(uint32_t address) {
    address &= NSL_ADDR26;
    trace_read(address);
    return (bus_read_dword(address & ~3u) >> ((address & 2) * 8)) & 0xFFFF;
}

uint32_t nsl_bus_read32(uint32_t address) {
    address &= NSL_MASK26;
    trace_read(address);
    return bus_read_dword(address);
}

void nsl_bus_write8(uint32_t address, uint32_t value) {
    address &= NSL_ADDR26;
    trace_write(address, value);
    if (address >= 0x100000 && address < 0x120000) {
        nsl_machine.ram[address - 0x100000] = (uint8_t)value;
        return;
    }
    unsigned shift = (address & 3) * 8;
    /* ARM2 STRB drives the byte on all four lanes; the decoder enables one. */
    bus_write_dword(address & ~3u, (value & 0xFFu) * 0x01010101u, 0xFFu << shift);
}

void nsl_bus_write16(uint32_t address, uint32_t value) {
    address &= NSL_ADDR26;
    trace_write(address, value);
    unsigned shift = (address & 2) * 8;
    bus_write_dword(address & ~3u, (value & 0xFFFFu) * 0x00010001u, 0xFFFFu << shift);
}

void nsl_bus_write32(uint32_t address, uint32_t value) {
    address &= NSL_MASK26;
    trace_write(address, value);
    bus_write_dword(address, value, 0xFFFFFFFFu);
}
