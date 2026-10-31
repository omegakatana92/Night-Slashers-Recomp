/* Compile-only stub for CI. */
#include "program_blocks.h"

void nsl_dispatch(void) {
    nsl_cpu_fault("CI stub: no translated program at %06X", nsl_cpu.pc);
}
