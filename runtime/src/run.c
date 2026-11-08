/*
 * Night Slashers native runtime: frame scheduler.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "nsl/run.h"
#include "nsl/cpu.h"
#include "nsl/machine.h"

#include <stdio.h>

void nsl_tick(unsigned instructions) {
    nsl_cpu.cycles += instructions;
}

/*
 * Runs the board for exactly one video frame of CPU time. A frame ends at the start
 * of VBLANK, where MAME's irq0_line_assert raises the ARM IRQ line; the game clears
 * it by writing 0x140000. The scheduler never requires the game to take the IRQ, so
 * code that runs with interrupts masked (EEPROM tests, POST) simply spans frames.
 */
int nsl_run_frame(void) {
    uint64_t frame_end = nsl_cpu.next_irq_cycle;

    while (nsl_cpu.cycles < frame_end) {
        if (nsl_cpu.fault || nsl_cpu.halted) {
            return -1;
        }
        if (nsl_machine.vblank_active && nsl_cpu.cycles >= nsl_machine.vblank_end_cycle) {
            nsl_machine_set_vblank(0);
        }
        nsl_cpu_check_irq();
        nsl_dispatch();
    }
    /* VBLANK begins: latch the frame, raise IRQ0 and the VBLANK input bit. */
    nsl_cpu.next_irq_cycle = frame_end + NSL_FRAME_CYCLES;
    nsl_cpu.pending_irq = 1;
    nsl_cpu.vblank_count++;
    nsl_machine_set_vblank(1);
    nsl_machine.vblank_end_cycle = frame_end + NSL_VBLANK_CYCLES;
    nsl_video_end_frame();
    nsl_machine.frame_count++;
    return 0;
}

int nsl_run_frames(int count) {
    for (int frame = 0; frame < count; frame++) {
        if (nsl_run_frame() != 0) {
            fprintf(stderr, "frame %d failed: %s\n", frame, nsl_cpu.fault_text);
            return -1;
        }
    }
    return 0;
}
