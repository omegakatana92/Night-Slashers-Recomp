/*
 * Night Slashers native runtime: bus, DECO ACE palette, sprite/tile RAM, EEPROM, inputs.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "nsl/machine.h"
#include "nsl/cpu.h"
#include "deco104_table.h"

#include <stdio.h>
#include <string.h>

NslMachine nsl_machine;

static uint8_t eeprom_do;
static int eeprom_cs, eeprom_clk, eeprom_di;
enum { EE_RESET, EE_START, EE_COMMAND, EE_READ, EE_WRITE, EE_COMPLETE };
static int eeprom_state;
static int eeprom_bits;
static unsigned eeprom_accum;
static unsigned eeprom_address;
static uint16_t eeprom_shift;
static uint16_t eeprom_data[64];

static uint16_t bitswap16_pos(uint16_t value, const unsigned char *positions) {
    uint16_t result = 0;
    for (int index = 0; index < 16; index++) {
        result = (uint16_t)((result << 1) | ((value >> positions[index]) & 1));
    }
    return result;
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

void nsl_machine_init(uint8_t *decrypted_program) {    memset(&nsl_machine, 0, sizeof(nsl_machine));
    nsl_machine.program = decrypted_program;
    nsl_machine.volume = 0x1100;
    for (unsigned bank = 0; bank < 2; bank++) {
        for (unsigned index = 0; index < 0x80; index++) {
            nsl_machine.deco_ram[bank][index] = 0xFFFF;
        }
    }
    nsl_machine.deco_bank = 0;
    nsl_machine.deco_latchaddr = 0xFFFF;
    eeprom_do = 1;
    for (unsigned index = 0; index < 64; index++) {
        eeprom_data[index] = 0xFFFF;
    }
    eeprom_state = EE_RESET;
    for (unsigned index = 0; index < sizeof(nsl_machine.eeprom); index++) {
        nsl_machine.eeprom[index] = 0xFF;
    }
    nsl_machine_set_default_inputs();
}

int nsl_machine_load_eeprom(const char *path) {
    FILE *file = fopen(path, "rb");
    if (!file) {
        return -1;
    }
    unsigned char raw[128];
    size_t got = fread(raw, 1, sizeof(raw), file);
    fclose(file);
    if (got != sizeof(raw)) {
        return -1;
    }
    for (unsigned index = 0; index < 64; index++) {
        eeprom_data[index] = (uint16_t)(raw[index * 2] | (raw[index * 2 + 1] << 8));
    }
    return 0;
}

/* ---- 93C46 serial EEPROM (MAME's eeprom_serial_93cxx_device) -------------- */

static void eeprom_clock(int rising) {
    if (eeprom_state == EE_RESET) {
        return;
    }
    if (!rising) {
        return;
    }
    switch (eeprom_state) {
    case EE_START:
        if (eeprom_di) {
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
            if (nsl_machine.trace_eeprom) {
                fprintf(stderr, "eeprom cmd %u addr %02X\n", command, eeprom_address);
            }
            if (command == 2) {                 /* read */
                eeprom_shift = 0;
                eeprom_do = 0;                  /* the dummy bit before the first data bit */
                eeprom_state = EE_READ;
            } else if (command == 1) {          /* write */
                eeprom_accum = 0;
                eeprom_state = EE_WRITE;
            } else if (command == 3) {          /* erase */
                eeprom_data[eeprom_address] = 0xFFFF;
                eeprom_do = 1;
                eeprom_state = EE_COMPLETE;
            } else {                             /* lock/unlock/erase-all: unsupported */
                eeprom_do = 1;
                eeprom_state = EE_COMPLETE;
            }
        }
        break;
    case EE_READ:
        if (eeprom_bits == 0) {
            eeprom_shift = eeprom_data[eeprom_address];
            eeprom_do = (eeprom_shift >> 15) & 1;
            eeprom_bits = 1;
            if (nsl_machine.trace_eeprom) {
                fprintf(stderr, "eeprom read %02X = %04X\n", eeprom_address, eeprom_shift);
            }
        } else {
            eeprom_shift <<= 1;
            eeprom_do = (eeprom_shift >> 15) & 1;
            eeprom_bits++;
        }
        break;
    case EE_WRITE:
        eeprom_accum = (eeprom_accum << 1) | (unsigned)eeprom_di;
        if (++eeprom_bits == 16) {
            static const unsigned char swap[16] = { 8,9,10,11,12,13,14,15,0,1,2,3,4,5,6,7 };
            eeprom_data[eeprom_address] = bitswap16_pos((uint16_t)eeprom_accum, swap);
            eeprom_do = 1;
            eeprom_state = EE_COMPLETE;
            if (nsl_machine.trace_eeprom) {
                fprintf(stderr, "eeprom write %02X = %04X\n", eeprom_address,
                        eeprom_data[eeprom_address]);
            }
        }
        break;
    default:
        break;
    }
}

static void eeprom_write(uint8_t value) {
    int cs = (value >> 6) & 1;
    int clk = (value >> 5) & 1;
    int di = (value >> 4) & 1;
    /* MAME's fghthist eeprom_w clocks first, then sets DI, then sets CS; the
     * EEPROM samples the DI that was present before this write. */
    if (clk != eeprom_clk) {
        eeprom_clk = clk;
        eeprom_clock(clk);
    }
    eeprom_di = di;
    if (cs != eeprom_cs) {
        eeprom_cs = cs;
        if (cs) {
            eeprom_state = EE_START;
            eeprom_bits = 0;
            eeprom_accum = 0;
            eeprom_do = 1;
        } else {
            eeprom_state = EE_RESET;
            eeprom_do = 1;
        }
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
        data = (uint16_t)(eeprom_do ? 1 : 0);
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
    if (nsl_machine.trace_deco) {
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

/* ---- bus --------------------------------------------------------------- */

static void trace_read(uint32_t address) {
    if (nsl_machine.trace_bus && address != nsl_machine.last_read_address) {
        fprintf(stderr, "read  %06X\n", address);
        nsl_machine.last_read_address = address;
    }
}

static void trace_write(uint32_t address, uint32_t value) {
    if (nsl_machine.trace_bus && address != nsl_machine.last_write_address) {
        fprintf(stderr, "write %06X = %08X\n", address, value);
        nsl_machine.last_write_address = address;
    }
}

uint32_t nsl_bus_read8(uint32_t address) {
    address &= NSL_MASK26;
    trace_read(address);
    if (address < NSL_PROGRAM_SIZE) {
        return nsl_machine.program[address];
    }
    if (address >= 0x100000 && address < 0x120000) {
        return nsl_machine.ram[address - 0x100000];
    }
    if (address >= 0x120000 && address < 0x120100) {
        if (address == 0x120020) return nsl_machine.inputs_in0 & 0xFF;
        if (address == 0x120021) return (nsl_machine.inputs_in0 >> 8) & 0xFF;
        if (address == 0x120024) return nsl_machine.inputs_in1 & 0xFF;
        if (address == 0x120025) return (nsl_machine.inputs_in1 >> 8) & 0xFF;
        if (address == 0x120028) return 0xFE | eeprom_do;
        return 0;
    }
    if (address == 0x150000) {
        return 0xFE | eeprom_do;
    }
    if (address >= 0x163000 && address < 0x1630A0) {
        return nsl_machine.ace[address - 0x163000];
    }
    if (address >= 0x168000 && address < 0x16A000) {
        return nsl_machine.palette_buffer[(address - 0x168000) >> 1] & 0xFF;
    }
    if (address >= 0x170000 && address < 0x172000) {
        return nsl_machine.spriteram[0][address - 0x170000];
    }
    if (address >= 0x178000 && address < 0x17A000) {
        return nsl_machine.spriteram[1][address - 0x178000];
    }
    if (address >= 0x182000 && address < 0x186000) {
        return nsl_machine.tilevram[(address - 0x182000) >> 13][(address - 0x182000) & 0x1FFF];
    }
    if (address >= 0x192000 && address < 0x196000) {
        return nsl_machine.rowscroll[(address - 0x192000) >> 13][(address - 0x192000) & 0x1FFF];
    }
    if (address >= 0x1A0000 && address < 0x1A0020) {
        return nsl_machine.tilecontrol[0][address - 0x1A0000];
    }
    if (address >= 0x1C2000 && address < 0x1C6000) {
        return nsl_machine.tilevram[2 + ((address - 0x1C2000) >> 13)][(address - 0x1C2000) & 0x1FFF];
    }
    if (address >= 0x1D2000 && address < 0x1D6000) {
        return nsl_machine.rowscroll[2 + ((address - 0x1D2000) >> 13)][(address - 0x1D2000) & 0x1FFF];
    }
    if (address >= 0x1E0000 && address < 0x1E0020) {
        return nsl_machine.tilecontrol[1][address - 0x1E0000];
    }
    if (address >= 0x200000 && address < 0x208000) {
        return deco104_read(address) & 0xFF;
    }
    nsl_machine.unmapped_reads++;
    return 0xFF;
}

uint32_t nsl_bus_read16(uint32_t address) {
    address &= NSL_MASK26;
    if (address >= 0x200000 && address < 0x208000) {
        return deco104_read(address);
    }
    if (address >= 0x168000 && address < 0x16A000) {
        return nsl_machine.palette_buffer[(address - 0x168000) >> 1];
    }
    return nsl_bus_read8(address) | (nsl_bus_read8(address + 1) << 8);
}

uint32_t nsl_bus_read32(uint32_t address) {
    address &= NSL_MASK26;
    if (address < NSL_PROGRAM_SIZE) {
        uint32_t value;
        memcpy(&value, &nsl_machine.program[address & ~3u], 4);
        return value;
    }
    if (address >= 0x100000 && address < 0x120000) {
        uint32_t value;
        memcpy(&value, &nsl_machine.ram[(address - 0x100000) & ~3u], 4);
        return value;
    }
    if (address == 0x120020) {
        return nsl_machine.inputs_in0;
    }
    if (address == 0x120024) {
        return nsl_machine.inputs_in1;
    }
    if (address >= 0x200000 && address < 0x208000) {
        return ((deco104_read(address) & 0xFFFFu) << 16) | 0xFFFFu;
    }
    return nsl_bus_read8(address) | (nsl_bus_read8(address + 1) << 8)
         | (nsl_bus_read8(address + 2) << 16) | (nsl_bus_read8(address + 3) << 24);
}

void nsl_bus_write8(uint32_t address, uint32_t value) {
    address &= NSL_MASK26;
    trace_write(address, value);
    if (address >= 0x100000 && address < 0x120000) {
        nsl_machine.ram[address - 0x100000] = (uint8_t)value;
        return;
    }
    if (address == 0x140000) { nsl_vblank_ack(); return; }
    if (address == 0x150000) { eeprom_write((uint8_t)value); return; }
    if (address == 0x150001) { nsl_machine.volume = value & 0xFF; return; }
    if (address >= 0x163000 && address < 0x1630A0) {
        nsl_machine.ace[address - 0x163000] = (uint8_t)value;
        return;
    }
    if (address >= 0x168000 && address < 0x16A000) {
        unsigned index = (address - 0x168000) >> 1;
        if (address & 1) {
            nsl_machine.palette_buffer[index] = (uint16_t)((nsl_machine.palette_buffer[index] & 0x00FF) | ((value & 0xFF) << 8));
        } else {
            nsl_machine.palette_buffer[index] = (uint16_t)((nsl_machine.palette_buffer[index] & 0xFF00) | (value & 0xFF));
        }
        return;
    }
    if (address == 0x164000) { nsl_machine.color_bank[0] = (uint8_t)value; return; }
    if (address == 0x164004) { nsl_machine.color_bank[1] = (uint8_t)value; return; }
    if (address == 0x164008) { nsl_machine.color_bank[2] = (uint8_t)value; return; }
    if (address >= 0x170000 && address < 0x172000) {
        nsl_machine.spriteram[0][address - 0x170000] = (uint8_t)value;
        return;
    }
    if (address >= 0x178000 && address < 0x17A000) {
        nsl_machine.spriteram[1][address - 0x178000] = (uint8_t)value;
        return;
    }
    if (address >= 0x174010 && address < 0x174014) {
        nsl_sprite_dma(0, (uint32_t)value);
        return;
    }
    if (address >= 0x17C010 && address < 0x17C014) {
        nsl_sprite_dma(1, (uint32_t)value);
        return;
    }
    if (address >= 0x182000 && address < 0x186000) {
        nsl_machine.tilevram[(address - 0x182000) >> 13][(address - 0x182000) & 0x1FFF] = (uint8_t)value;
        return;
    }
    if (address >= 0x192000 && address < 0x196000) {
        nsl_machine.rowscroll[(address - 0x192000) >> 13][(address - 0x192000) & 0x1FFF] = (uint8_t)value;
        return;
    }
    if (address >= 0x1A0000 && address < 0x1A0020) {
        nsl_machine.tilecontrol[0][address - 0x1A0000] = (uint8_t)value;
        return;
    }
    if (address >= 0x1C2000 && address < 0x1C6000) {
        nsl_machine.tilevram[2 + ((address - 0x1C2000) >> 13)][(address - 0x1C2000) & 0x1FFF] = (uint8_t)value;
        return;
    }
    if (address >= 0x1D2000 && address < 0x1D6000) {
        nsl_machine.rowscroll[2 + ((address - 0x1D2000) >> 13)][(address - 0x1D2000) & 0x1FFF] = (uint8_t)value;
        return;
    }
    if (address >= 0x1E0000 && address < 0x1E0020) {
        nsl_machine.tilecontrol[1][address - 0x1E0000] = (uint8_t)value;
        return;
    }
    if (address == 0x16C008 || address == 0x16C009 || address == 0x16C00A || address == 0x16C00B) {
        nsl_palette_dma(nsl_bus_read32(0x16C008));
        return;
    }
    nsl_machine.unmapped_writes++;
}

void nsl_bus_write16(uint32_t address, uint32_t value) {
    address &= NSL_MASK26;
    if (address >= 0x168000 && address < 0x16A000) {
        nsl_machine.palette_buffer[(address - 0x168000) >> 1] = (uint16_t)value;
        return;
    }
    nsl_bus_write8(address, value & 0xFF);
    nsl_bus_write8(address + 1, (value >> 8) & 0xFF);
}

void nsl_bus_write32(uint32_t address, uint32_t value) {
    address &= NSL_MASK26;
    if (address >= 0x200000 && address < 0x208000) {
        deco104_write(address, (uint16_t)(value >> 16));
        return;
    }
    if (address >= 0x100000 && address < 0x120000) {
        memcpy(&nsl_machine.ram[(address - 0x100000) & ~3u], &value, 4);
        return;
    }
    if (address == 0x150000) {
        eeprom_write((uint8_t)value);
        return;
    }
    if (address == 0x140000) {
        nsl_vblank_ack();
        return;
    }
    if (address >= 0x164000 && address < 0x16400C) {
        nsl_machine.color_bank[(address - 0x164000) >> 2] = (uint8_t)value;
        return;
    }
    if (address == 0x16C008) {
        nsl_palette_dma(value);
        return;
    }
    if (address >= 0x174010 && address < 0x174014) {
        nsl_sprite_dma(0, value);
        return;
    }
    if (address >= 0x17C010 && address < 0x17C014) {
        nsl_sprite_dma(1, value);
        return;
    }
    nsl_bus_write8(address, value & 0xFF);
    nsl_bus_write8(address + 1, (value >> 8) & 0xFF);
    nsl_bus_write8(address + 2, (value >> 16) & 0xFF);
    nsl_bus_write8(address + 3, (value >> 24) & 0xFF);
}

/* ---- board devices ----------------------------------------------------- */

void nsl_vblank_ack(void) {
    nsl_machine.vblank_ack_count++;
}

void nsl_palette_dma(uint32_t source) {
    (void)source;
    memcpy(nsl_machine.palette, nsl_machine.palette_buffer, sizeof(nsl_machine.palette));
    nsl_machine.palette_dma_count++;
}

void nsl_sprite_dma(unsigned which, uint32_t source) {
    (void)source;
    if (which < 2) {
        memcpy(nsl_machine.spritebuffer[which], nsl_machine.spriteram[which], 0x2000);
        nsl_machine.sprite_dma_count[which]++;
    }
}

void nsl_ace_write(unsigned index, uint32_t value) {
    if (index < sizeof(nsl_machine.ace)) {
        nsl_machine.ace[index] = (uint8_t)value;
    }
}

uint32_t nsl_ace_read(unsigned index) {
    return index < sizeof(nsl_machine.ace) ? nsl_machine.ace[index] : 0;
}
