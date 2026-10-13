/* Simulator stand-in for the ping-pong FX's FREE SDRAM buffer (_pingpong_ram = 0x01E80000 on the device, set by
 * build_slot.py from fx.json "ram"). On the device it is neither loaded nor zeroed, so here it is filled with
 * full-scale pseudo-random garbage before main(): any stale read shows up in pingpong_check.py. */
#include <stdint.h>

#define LINES 3                                 /* stereo L, stereo R, mono */
#define RING 65536

int16_t pingpong_ram[LINES * RING];

__attribute__((constructor)) static void garbage(void)
{
    uint32_t x = 0x12345678u;
    for (int i = 0; i < LINES * RING; i++) {
        x = x * 1664525u + 1013904223u;
        pingpong_ram[i] = (int16_t)(x >> 16);
    }
}
