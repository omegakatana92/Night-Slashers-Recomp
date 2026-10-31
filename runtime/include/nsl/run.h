/*
 * Night Slashers native runtime: frame scheduler.
 *
 * The DE156 runs at 28 MHz / 4 = 7 MHz. MAME's screen is 442 x 274 pixels at the
 * 7 MHz pixel clock, so a frame is 121,050 DE156 clocks and 57.828 Hz. The vblank
 * interrupt is raised once per frame, as MAME's set_vblank_int() does. The
 * translated code counts whole instructions; the count is a timing model, not a
 * per-instruction emulation.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef NSL_RUN_H
#define NSL_RUN_H

#include <stdint.h>

int nsl_run_frame(void);
int nsl_run_frames(int count);
void nsl_tick(unsigned instructions);

#endif
