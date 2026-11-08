/*
 * Night Slashers native runtime: fallback for code the static analysis missed.
 *
 * The ahead-of-time translator only sees code reachable through direct branches and
 * the indirect targets it can resolve. When the game jumps through a run-time
 * function pointer into code that was not translated, the dispatcher calls
 * nsl_interp_run(), which executes instructions with exactly the semantics the
 * translator emits (same helpers, same flag rules) until control reaches a
 * translated block again. Every such entry address is recorded so
 * tools/recompile.py --seeds can translate it ahead of time on the next build;
 * release builds are seeded from recorded play sessions.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "nsl/cpu.h"
#include "nsl/interp.h"
#include "nsl/machine.h"
#include "nsl/run.h"

#include <stdio.h>
#include <string.h>

#define MAX_MISSED 4096
static uint32_t g_missed[MAX_MISSED];
static unsigned g_missed_count;
static uint64_t g_interpreted;

uint64_t nsl_interp_instruction_count(void) { return g_interpreted; }

unsigned nsl_interp_missed(const uint32_t **list) {
    *list = g_missed;
    return g_missed_count;
}

static void note_missed(uint32_t pc) {
    for (unsigned i = 0; i < g_missed_count; i++) {
        if (g_missed[i] == pc) return;
    }
    if (g_missed_count < MAX_MISSED) g_missed[g_missed_count++] = pc;
}

static int condition_passes(uint32_t cond) {
    switch (cond) {
    case 0x0: return nsl_cpu.z;
    case 0x1: return !nsl_cpu.z;
    case 0x2: return nsl_cpu.c;
    case 0x3: return !nsl_cpu.c;
    case 0x4: return nsl_cpu.n;
    case 0x5: return !nsl_cpu.n;
    case 0x6: return nsl_cpu.v;
    case 0x7: return !nsl_cpu.v;
    case 0x8: return nsl_cpu.c && !nsl_cpu.z;
    case 0x9: return !nsl_cpu.c || nsl_cpu.z;
    case 0xA: return nsl_cpu.n == nsl_cpu.v;
    case 0xB: return nsl_cpu.n != nsl_cpu.v;
    case 0xC: return !nsl_cpu.z && nsl_cpu.n == nsl_cpu.v;
    case 0xD: return nsl_cpu.z || nsl_cpu.n != nsl_cpu.v;
    case 0xE: return 1;
    default: return 0;
    }
}

/* operand2 for data processing; updates nsl_cpu.shift_carry like the emitted code */
static uint32_t operand2(uint32_t word, uint32_t address, uint32_t *carry) {
    uint32_t pc_value = (address + 8) & NSL_ADDR26;
    if (word & (1u << 25)) {
        uint32_t imm = word & 0xFF;
        uint32_t rotation = ((word >> 8) & 0xF) * 2;
        uint32_t value = rotation ? ((imm >> rotation) | (imm << (32 - rotation))) : imm;
        *carry = rotation ? (value >> 31) & 1 : nsl_cpu.c;
        return value;
    }
    uint32_t rm = word & 0xF;
    uint32_t shift = (word >> 5) & 3;
    uint32_t source = rm == 15 ? nsl_r15(pc_value) : nsl_cpu.r[rm];
    nsl_cpu.shift_carry = nsl_cpu.c;
    uint32_t value;
    if (word & (1u << 4)) {
        uint32_t rs = (word >> 8) & 0xF;
        uint32_t amount = rs == 15 ? nsl_r15(pc_value) : nsl_cpu.r[rs];
        value = nsl_shift_reg(source, shift, amount);
    } else {
        uint32_t amount = (word >> 7) & 0x1F;
        value = (shift == 0 && amount == 0) ? source : nsl_shift_imm(source, shift, amount);
    }
    *carry = nsl_cpu.shift_carry;
    return value;
}

/* Executes one instruction. Returns 1 if it changed the flow of control. */
static int execute(uint32_t address, uint32_t word) {
    uint32_t next = (address + 4) & NSL_ADDR26;
    uint32_t pc_value = (address + 8) & NSL_ADDR26;
    nsl_cpu.pc = next;
    if (!condition_passes(word >> 28)) return 0;

    if ((word & 0x0C000000u) == 0) {
        if ((word & 0x0FC000F0u) == 0x00000090u) {                     /* MUL/MLA */
            uint32_t rd = (word >> 16) & 0xF, rn = (word >> 12) & 0xF;
            uint32_t rs = (word >> 8) & 0xF, rm = word & 0xF;
            int accumulate = (word >> 21) & 1, set_flags = (word >> 20) & 1;
            nsl_cpu.r[rd] = nsl_mul(nsl_cpu.r[rm], nsl_cpu.r[rs], accumulate ? nsl_cpu.r[rn] : 0,
                                    accumulate, set_flags);
            return 0;
        }
        if ((word & 0x0FBF0FFFu) == 0x010F0000u || (word & 0x0FB0FFF0u) == 0x0120F000u
            || (word & 0x0FFFFFF0u) == 0x012FFF10u) {
            nsl_cpu_note_undefined(address, word);
            return 1;
        }
        uint32_t opcode = (word >> 21) & 0xF;
        int set_flags = (word >> 20) & 1;
        uint32_t rn_index = (word >> 16) & 0xF, rd = (word >> 12) & 0xF;
        uint32_t carry;
        if (set_flags && (opcode <= 1 || (opcode >= 8 && opcode <= 9) || opcode >= 0xC)) {
            nsl_cpu.shift_carry = nsl_cpu.c;
        }
        uint32_t op2 = operand2(word, address, &carry);
        uint32_t rn = rn_index == 15 ? pc_value : nsl_cpu.r[rn_index];
        uint32_t result;
        int is_test = opcode >= 8 && opcode <= 0xB;
        switch (opcode) {
        case 0x0: result = nsl_logical(rn & op2, set_flags, carry); break;
        case 0x1: result = nsl_logical(rn ^ op2, set_flags, carry); break;
        case 0x2: result = nsl_sub(rn, op2, 0, set_flags); break;
        case 0x3: result = nsl_sub(op2, rn, 0, set_flags); break;
        case 0x4: result = nsl_add(rn, op2, 0, set_flags); break;
        case 0x5: result = nsl_add(rn, op2, nsl_cpu.c, set_flags); break;
        case 0x6: result = nsl_sub(rn, op2, nsl_cpu.c, set_flags); break;
        case 0x7: result = nsl_sub(op2, rn, nsl_cpu.c, set_flags); break;
        case 0x8: result = nsl_logical(rn & op2, 1, carry); break;
        case 0x9: result = nsl_logical(rn ^ op2, 1, carry); break;
        case 0xA: result = nsl_sub(rn, op2, 0, 1); break;
        case 0xB: result = nsl_add(rn, op2, 0, 1); break;
        case 0xC: result = nsl_logical(rn | op2, set_flags, carry); break;
        case 0xD: result = nsl_logical(op2, set_flags, carry); break;
        case 0xE: result = nsl_logical(rn & ~op2, set_flags, carry); break;
        default:  result = nsl_logical(~op2, set_flags, carry); break;
        }
        if (is_test) {
            if (rd == 15) nsl_psr_test(result);
            return rd == 15;
        }
        if (rd == 15) {
            if (set_flags) nsl_write_pc_and_psr(result);
            else nsl_cpu.pc = result & NSL_MASK26;
            return 1;
        }
        nsl_cpu.r[rd] = result;
        return 0;
    }

    if ((word & 0x0C000000u) == 0x04000000u) {                          /* LDR/STR */
        int reg_offset = (word >> 25) & 1, pre = (word >> 24) & 1, up = (word >> 23) & 1;
        int byte = (word >> 22) & 1, writeback = (word >> 21) & 1, load = (word >> 20) & 1;
        uint32_t rn = (word >> 16) & 0xF, rd = (word >> 12) & 0xF;
        uint32_t base = rn == 15 ? pc_value : nsl_cpu.r[rn];
        uint32_t offset;
        if (reg_offset) {
            uint32_t rm = word & 0xF, shift = (word >> 5) & 3, amount = (word >> 7) & 0x1F;
            uint32_t source = rm == 15 ? nsl_r15(pc_value) : nsl_cpu.r[rm];
            offset = (shift == 0 && amount == 0) ? source : nsl_shift_imm(source, shift, amount);
        } else {
            offset = word & 0xFFF;
        }
        uint32_t moved = up ? base + offset : base - offset;
        uint32_t ea = (pre ? moved : base) & NSL_ADDR26;
        int do_writeback = pre ? writeback : 1;
        if (load) {
            uint32_t value = byte ? nsl_read8(ea) : nsl_read32(ea);
            if (rd == 15) nsl_cpu.pc = value & NSL_MASK26;
            else nsl_cpu.r[rd] = value;
            if (do_writeback) nsl_cpu.r[rn] = moved;
            return rd == 15;
        }
        uint32_t value = rd == 15 ? nsl_r15(pc_value + 4) : nsl_cpu.r[rd];
        if (byte) nsl_write8(ea, value); else nsl_write32(ea, value);
        if (do_writeback) nsl_cpu.r[rn] = moved;
        return 0;
    }

    if ((word & 0x0E000000u) == 0x08000000u) {                          /* LDM/STM */
        int pre = (word >> 24) & 1, up = (word >> 23) & 1, user = (word >> 22) & 1;
        int writeback = (word >> 21) & 1, load = (word >> 20) & 1;
        uint32_t rn = (word >> 16) & 0xF, list = word & 0xFFFF;
        uint32_t base = rn == 15 ? nsl_r15(pc_value) : nsl_cpu.r[rn];
        nsl_ldm(base, list, load, pre, up, writeback, user, pc_value, rn);
        return load && (list & 0x8000u);
    }

    if ((word & 0x0E000000u) == 0x0A000000u) {                          /* B/BL */
        int32_t offset = (int32_t)((word & 0xFFFFFF) << 8) >> 6;
        if (word & (1u << 24)) nsl_cpu.r[14] = nsl_r15(next);
        nsl_cpu.pc = (uint32_t)(pc_value + offset) & NSL_ADDR26;
        return 1;
    }

    if ((word & 0x0F000000u) == 0x0E000000u && ((word >> 8) & 0xF) == 0) {   /* DE156 cp0 */
        uint32_t creg = (word >> 12) & 0xF;
        if (word & (1u << 4)) {
            if (word & (1u << 20)) nsl_cpu.r[(word >> 12) & 0xF] = nsl_copro_read(creg);
            else nsl_copro_write(creg, nsl_cpu.r[(word >> 12) & 0xF]);
        } else {
            nsl_copro_divide();
        }
        return 0;
    }

    if ((word & 0x0F000000u) == 0x0F000000u) {
        nsl_cpu_fault("SWI %06X at %06X", word & 0xFFFFFF, address);
        return 1;
    }
    nsl_cpu_note_undefined(address, word);
    return 1;
}

void nsl_interp_run(void) {
    uint32_t pc = nsl_cpu.pc & NSL_MASK26;
    note_missed(pc);
    for (int count = 0; count < 256; count++) {
        if (count > 0 && nsl_has_block(nsl_cpu.pc & NSL_MASK26)) return;
        uint32_t address = nsl_cpu.pc & NSL_MASK26;
        uint32_t word = nsl_read32(address);
        nsl_tick(1);
        g_interpreted++;
        if (execute(address, word) || nsl_cpu.fault) return;
    }
}
