/*
 * Night Slashers native runtime: DE156 CPU helpers.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "nsl/cpu.h"
#include "nsl/machine.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

NslCpu nsl_cpu;

static void bank_save(void) {
    nsl_cpu.bank13[nsl_cpu.mode] = nsl_cpu.r[13];
    nsl_cpu.bank14[nsl_cpu.mode] = nsl_cpu.r[14];
}

static void bank_load(void) {
    nsl_cpu.r[13] = nsl_cpu.bank13[nsl_cpu.mode];
    nsl_cpu.r[14] = nsl_cpu.bank14[nsl_cpu.mode];
}

static void switch_mode(uint32_t new_mode) {
    if (new_mode == nsl_cpu.mode) {
        return;
    }
    if (nsl_cpu.mode == 1) {
        for (int index = 0; index < 5; index++) {
            nsl_cpu.fiq_r8_12[index] = nsl_cpu.r[8 + index];
            nsl_cpu.r[8 + index] = nsl_cpu.user_r8_12[index];
        }
    }
    bank_save();
    nsl_cpu.mode = new_mode & 3;
    bank_load();
    if (nsl_cpu.mode == 1) {
        for (int index = 0; index < 5; index++) {
            nsl_cpu.user_r8_12[index] = nsl_cpu.r[8 + index];
            nsl_cpu.r[8 + index] = nsl_cpu.fiq_r8_12[index];
        }
    }
}

void nsl_cpu_reset(void) {
    memset(&nsl_cpu, 0, sizeof(nsl_cpu));
    nsl_cpu.mode = 3;          /* ARM reset enters SVC mode */
    nsl_cpu.i = 1;
    nsl_cpu.f = 1;
    nsl_cpu.next_irq_cycle = NSL_VBLANK_START_CYCLES;
}

uint32_t nsl_get_psr(void) {
    return (nsl_cpu.n << 31) | (nsl_cpu.z << 30) | (nsl_cpu.c << 29) | (nsl_cpu.v << 28)
         | (nsl_cpu.i << 7) | (nsl_cpu.f << 6) | (nsl_cpu.mode & 3);
}

void nsl_set_psr(uint32_t value) {
    uint32_t new_mode = value & 3;
    if (new_mode != nsl_cpu.mode) {
        switch_mode(new_mode);
    }
    nsl_cpu.n = (value >> 31) & 1;
    nsl_cpu.z = (value >> 30) & 1;
    nsl_cpu.c = (value >> 29) & 1;
    nsl_cpu.v = (value >> 28) & 1;
    nsl_cpu.i = (value >> 7) & 1;
    nsl_cpu.f = (value >> 6) & 1;
}

void nsl_psr_test(uint32_t result) {
    if (nsl_cpu.mode == 0) {
        nsl_cpu.n = (result >> 31) & 1;
        nsl_cpu.z = (result >> 30) & 1;
        nsl_cpu.c = (result >> 29) & 1;
        nsl_cpu.v = (result >> 28) & 1;
        return;
    }
    nsl_set_psr(result);
}

void nsl_write_pc_and_psr(uint32_t value) {
    uint32_t old_mode = nsl_cpu.mode;
    uint32_t new_mode = value & 3;
    if (nsl_cpu.mode == 0) {
        /* user mode: I, F and the mode bits are protected; only NZCV are written */
        nsl_cpu.n = (value >> 31) & 1;
        nsl_cpu.z = (value >> 30) & 1;
        nsl_cpu.c = (value >> 29) & 1;
        nsl_cpu.v = (value >> 28) & 1;
        nsl_cpu.pc = value & NSL_MASK26;
        return;
    }
    if (new_mode != nsl_cpu.mode) {
        switch_mode(new_mode);
    }
    nsl_cpu.n = (value >> 31) & 1;
    nsl_cpu.z = (value >> 30) & 1;
    nsl_cpu.c = (value >> 29) & 1;
    nsl_cpu.v = (value >> 28) & 1;
    nsl_cpu.i = (value >> 27) & 1;
    nsl_cpu.f = (value >> 26) & 1;
    nsl_cpu.pc = value & NSL_MASK26;
    if (old_mode == 2 && nsl_cpu.mode != 2) {
        if (nsl_cpu.trace_irq) {
            fprintf(stderr, "IRQ leave to %06X mode %u sp %08X r1 %08X\n",
                    nsl_cpu.pc, nsl_cpu.mode, nsl_cpu.r[13], nsl_cpu.r[1]);
        }
    }
}

uint32_t nsl_r15(uint32_t pc) {
    return (pc & NSL_MASK26)
         | (nsl_cpu.n << 31) | (nsl_cpu.z << 30) | (nsl_cpu.c << 29) | (nsl_cpu.v << 28)
         | (nsl_cpu.i << 27) | (nsl_cpu.f << 26) | (nsl_cpu.mode & 3);
}

void nsl_cpu_fault(const char *format, ...) {
    if (!nsl_cpu.fault) {
        va_list args;
        va_start(args, format);
        vsnprintf(nsl_cpu.fault_text, sizeof(nsl_cpu.fault_text), format, args);
        va_end(args);
        nsl_cpu.fault = 1;
        nsl_cpu.halted = 1;
        fprintf(stderr, "Night Slashers CPU fault at %06X: %s\n",
                nsl_cpu.pc, nsl_cpu.fault_text);
    }
}

void nsl_cpu_note_undefined(uint32_t address, uint32_t instruction) {
    nsl_cpu_fault("untranslated instruction %08X at %06X", instruction, address);
}

void nsl_cpu_take_irq(void) {
    uint32_t cpsr = nsl_get_psr();
    uint32_t link = (nsl_cpu.pc + 4) & NSL_MASK26;
    bank_save();
    nsl_cpu.mode = 2;
    bank_load();
    nsl_cpu.r[14] = (link & NSL_MASK26) | (cpsr & 0xF0000000u)
                  | ((cpsr & (NSL_PSR_I | NSL_PSR_F)) << 20) | (cpsr & 3);
    nsl_cpu.i = 1;
    nsl_cpu.pc = 0x18;
    nsl_cpu.irq_count++;
    nsl_cpu.cycles += 4;
    if (nsl_cpu.trace_irq) {
        fprintf(stderr, "IRQ enter from %06X -> lr %08X sp %08X mode %u r1 %08X\n",
                link, nsl_cpu.r[14], nsl_cpu.r[13], nsl_cpu.mode, nsl_cpu.r[1]);
    }
}

void nsl_cpu_take_fiq(void) {
    uint32_t cpsr = nsl_get_psr();
    uint32_t link = (nsl_cpu.pc + 4) & NSL_MASK26;
    bank_save();
    nsl_cpu.mode = 1;
    bank_load();
    nsl_cpu.r[14] = (link & NSL_MASK26) | (cpsr & 0xF0000000u)
                  | ((cpsr & (NSL_PSR_I | NSL_PSR_F)) << 20) | (cpsr & 3);
    nsl_cpu.i = 1;
    nsl_cpu.f = 1;
    nsl_cpu.pc = 0x1C;
    nsl_cpu.cycles += 4;
}

void nsl_cpu_check_irq(void) {
    if (nsl_cpu.pending_irq && !nsl_cpu.i) {
        nsl_cpu_take_irq();
    }
}

void nsl_set_nz(uint32_t value) {
    nsl_cpu.n = value >> 31;
    nsl_cpu.z = value == 0;
}

uint32_t nsl_add(uint32_t a, uint32_t b, uint32_t carry_in, int set_flags) {
    uint64_t wide = (uint64_t)a + b + carry_in;
    uint32_t result = (uint32_t)wide;
    if (set_flags) {
        nsl_cpu.c = (uint32_t)(wide >> 32) & 1;
        nsl_cpu.v = ((~(a ^ b) & (a ^ result)) >> 31) & 1;
        nsl_set_nz(result);
    }
    return result;
}

uint32_t nsl_sub(uint32_t a, uint32_t b, uint32_t carry_in, int set_flags) {
    uint64_t wide = (uint64_t)a - b - carry_in;
    uint32_t result = (uint32_t)wide;
    if (set_flags) {
        nsl_cpu.c = ((wide >> 63) & 1) ? 0 : 1;    /* ARM C is NOT borrow */
        nsl_cpu.v = (((a ^ b) & (a ^ result)) >> 31) & 1;
        nsl_set_nz(result);
    }
    return result;
}

uint32_t nsl_logical(uint32_t result, int set_flags, uint32_t carry) {
    if (set_flags) {
        nsl_set_nz(result);
        nsl_cpu.c = carry & 1;
    }
    return result;
}

uint32_t nsl_mul(uint32_t a, uint32_t b, uint32_t addend, int accumulate, int set_flags) {
    uint32_t result = a * b + (accumulate ? addend : 0);
    if (set_flags) {
        nsl_set_nz(result);
    }
    return result;
}

uint32_t nsl_shift_imm(uint32_t value, unsigned type, unsigned amount) {
    switch (type & 3) {
    case 0: /* LSL */
        if (amount == 0) {
            return value;
        }
        if (amount < 32) {
            nsl_cpu.shift_carry = (value >> (32 - amount)) & 1;
            return value << amount;
        }
        nsl_cpu.shift_carry = 0;
        return 0;
    case 1: /* LSR; amount 0 means 32 */
        if (amount == 0) {
            nsl_cpu.shift_carry = value >> 31;
            return 0;
        }
        nsl_cpu.shift_carry = (value >> (amount - 1)) & 1;
        return value >> amount;
    case 2: /* ASR; amount 0 means 32 */
        if (amount == 0) {
            nsl_cpu.shift_carry = value >> 31;
            return (value & 0x80000000u) ? 0xFFFFFFFFu : 0;
        }
        nsl_cpu.shift_carry = (value >> (amount - 1)) & 1;
        return (uint32_t)(((int32_t)value) >> amount);
    default: /* ROR; amount 0 means RRX */
        if (amount == 0) {
            uint32_t carry = nsl_cpu.shift_carry & 1;
            nsl_cpu.shift_carry = value & 1;
            return (carry << 31) | (value >> 1);
        }
        nsl_cpu.shift_carry = (value >> (amount - 1)) & 1;
        return (value >> amount) | (value << (32 - amount));
    }
}

uint32_t nsl_shift_reg(uint32_t value, unsigned type, uint32_t amount) {
    amount &= 0xFF;
    if (amount == 0) {
        return value;
    }
    switch (type & 3) {
    case 0:
        if (amount < 32) {
            nsl_cpu.shift_carry = (value >> (32 - amount)) & 1;
            return value << amount;
        }
        nsl_cpu.shift_carry = 0;
        return 0;
    case 1:
        if (amount < 32) {
            nsl_cpu.shift_carry = (value >> (amount - 1)) & 1;
            return value >> amount;
        }
        nsl_cpu.shift_carry = (amount == 32) ? (value >> 31) : 0;
        return 0;
    case 2:
        if (amount < 32) {
            nsl_cpu.shift_carry = (value >> (amount - 1)) & 1;
            return (uint32_t)(((int32_t)value) >> amount);
        }
        nsl_cpu.shift_carry = value >> 31;
        return (value & 0x80000000u) ? 0xFFFFFFFFu : 0;
    default:
        amount &= 31;
        if (amount == 0) {
            nsl_cpu.shift_carry = value >> 31;
            return value;
        }
        nsl_cpu.shift_carry = (value >> (amount - 1)) & 1;
        return (value >> amount) | (value << (32 - amount));
    }
}

int nsl_ldm_loaded_pc(uint32_t list, int load) {
    return load && (list & 0x8000);
}

/* LDM/STM, ARM2 26-bit semantics (MAME arm7core 26-bit path):
 * - LDM with R15 and S: the loaded word sets PC and the PSR bits the current mode
 *   may change (nsl_write_pc_and_psr); without S only the PC is loaded.
 * - LDM/STM with S and no R15 (or STM with S): transfer the user-bank registers.
 * - STM of R15 stores PC + 12 together with the PSR bits. */
static uint32_t *user_bank_reg(unsigned reg) {
    static uint32_t scratch;
    if (nsl_cpu.mode == 0) return &nsl_cpu.r[reg];
    if (reg == 13 || reg == 14) {
        scratch = 0;
        return reg == 13 ? &nsl_cpu.bank13[0] : &nsl_cpu.bank14[0];
    }
    if (reg >= 8 && reg <= 12 && nsl_cpu.mode == 1) return &nsl_cpu.user_r8_12[reg - 8];
    return &nsl_cpu.r[reg];
}

uint32_t nsl_ldm(uint32_t base, uint32_t list, int load, int pre, int up,
                 int writeback, int user, uint32_t pc_value, unsigned base_reg) {
    unsigned count = 0;
    for (unsigned index = 0; index < 16; index++) {
        if (list & (1u << index)) {
            count++;
        }
    }
    if (count == 0) {
        return base;
    }
    uint32_t first;
    if (pre && up) {
        first = base + 4;                 /* IB */
    } else if (pre && !up) {
        first = base - count * 4;         /* DB */
    } else if (!pre && up) {
        first = base;                     /* IA */
    } else {
        first = base - (count - 1) * 4;   /* DA */
    }
    int user_bank = user && !(load && (list & 0x8000u));
    uint32_t address = first;
    uint32_t loaded_pc = 0;
    int have_pc = 0;
    for (unsigned reg = 0; reg < 16; reg++) {
        if (!(list & (1u << reg))) {
            continue;
        }
        if (load) {
            uint32_t value = nsl_read32(address);
            if (reg == 15) {
                loaded_pc = value;
                have_pc = 1;
            } else if (user_bank) {
                *user_bank_reg(reg) = value;
            } else {
                nsl_cpu.r[reg] = value;
            }
        } else {
            uint32_t value;
            if (reg == 15) value = nsl_r15(pc_value + 4);
            else if (user_bank) value = *user_bank_reg(reg);
            else value = nsl_cpu.r[reg];
            nsl_write32(address, value);
        }
        address += 4;
    }
    uint32_t result = base;
    if (writeback && !(load && (list & (1u << (base_reg & 15))))) {
        result = up ? base + count * 4 : base - count * 4;
        if (base_reg < 15) {
            nsl_cpu.r[base_reg] = result;    /* before any mode change below */
        }
    }
    if (have_pc) {
        if (user) {
            nsl_write_pc_and_psr(loaded_pc);
        } else {
            nsl_cpu.pc = loaded_pc & NSL_MASK26;
        }
    }
    return result;
}

/* ---- DE156 coprocessor 0 ------------------------------------------------ */

static uint32_t bcd_to_decimal(uint32_t value) {
    uint32_t result = 0;
    uint32_t scale = 1;
    for (int digit = 0; digit < 8; digit++) {
        result += (value & 0xF) * scale;
        value >>= 4;
        scale *= 10;
    }
    return result;
}

static uint32_t decimal_to_bcd(uint32_t value) {
    uint32_t result = 0;
    for (int digit = 0; digit < 8; digit++) {
        result |= (value % 10) << (digit * 4);
        value /= 10;
    }
    return result;
}

void nsl_copro_write(unsigned reg, uint32_t value) {
    reg &= 15;
    nsl_cpu.copro[reg] = value;
    if (reg == 2) {
        uint32_t a = bcd_to_decimal(nsl_cpu.copro[0]);
        uint32_t b = bcd_to_decimal(nsl_cpu.copro[1]);
        switch (value) {
        case 0: nsl_cpu.copro[5] = decimal_to_bcd(a + b); break;
        case 1: nsl_cpu.copro[5] = decimal_to_bcd(a * b); break;
        case 3: nsl_cpu.copro[5] = decimal_to_bcd(a - b); break;
        default: break;
        }
    }
}

uint32_t nsl_copro_read(unsigned reg) {
    return nsl_cpu.copro[reg & 15];
}

void nsl_copro_divide(void) {
    if (nsl_cpu.copro[1] != 0) {
        nsl_cpu.copro[3] = nsl_cpu.copro[0] / nsl_cpu.copro[1];
        nsl_cpu.copro[4] = nsl_cpu.copro[0] % nsl_cpu.copro[1];
    } else {
        nsl_cpu.copro[3] = 0xFFFFFFFFu;
        nsl_cpu.copro[4] = 0xFFFFFFFFu;
    }
}
