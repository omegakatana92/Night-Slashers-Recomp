/*
 * Night Slashers native runtime: video.
 *
 * The full DECO32 renderer (two DECO16IC tile generators, row scroll, two
 * zooming sprite generators with DECO ACE palettes) is being brought up. Until
 * it is in place this clears the frame so the CPU and bus can be validated.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "nsl/machine.h"

#include <string.h>

void nsl_machine_render(void) {
    for (int y = 0; y < NSL_SCREEN_HEIGHT; y++) {
        for (int x = 0; x < NSL_SCREEN_WIDTH; x++) {
            nsl_machine.framebuffer[y][x][0] = 0;
            nsl_machine.framebuffer[y][x][1] = 0;
            nsl_machine.framebuffer[y][x][2] = 0;
        }
    }
}
