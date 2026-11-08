/* Compile-only stub for CI (no ROM, no translated program). */
#include "program_blocks.h"
#include "nsl/interp.h"

int nsl_has_block(uint32_t pc) { (void)pc; return 0; }

void nsl_dispatch(void) {
    nsl_cpu_fault("CI stub: no translated program at %06X", nsl_cpu.pc);
}
