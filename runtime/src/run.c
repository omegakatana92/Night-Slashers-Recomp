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

int nsl_run_frame(void) {
    uint32_t target = nsl_cpu.vblank_count + 1;
    uint64_t budget = nsl_cpu.cycles + NSL_FRAME_CYCLES * 8;

    while (nsl_cpu.vblank_count < target) {
        if (nsl_cpu.fault || nsl_cpu.halted) {
            return -1;
        }
        if (nsl_cpu.cycles > budget) {
            nsl_cpu_fault("frame did not reach vblank within %u cycles",
                          NSL_FRAME_CYCLES * 8);
            return -1;
        }
        if (nsl_cpu.cycles >= nsl_cpu.next_irq_cycle) {
            nsl_cpu.pending_irq = 1;
            nsl_cpu.next_irq_cycle += NSL_FRAME_CYCLES;
        }
        if (nsl_machine.vblank_active && nsl_cpu.cycles >= nsl_machine.vblank_end_cycle) {
            nsl_machine_set_vblank(0);
        }
        nsl_cpu_check_irq();
        nsl_dispatch();
    }
    nsl_machine.frame_count++;
    nsl_machine_set_default_inputs();
    nsl_machine_render();
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
