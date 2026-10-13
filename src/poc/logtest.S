/* FSK logger with a fixed test payload: proves the render -> tape -> decoder path.
 * The payload has the canarylog.S layout, so the decoder's canary view is exercised too:
 *   mask 0x5; region 0 at 0x00812340 = 0x00000000; region 2 at 0x02C00004 = 0xDEADBEEF; 1234 sweeps.
 * Registers: R0-R3, P0-P2, LC0/LT0/LB0 (scratch); P5 saved/restored.
 */
    .include "fsk.inc"

    .text
    .global _render
_render:
    [--SP] = P5;
    P5 = R0;                    /* this */
    P1 = R1;                    /* out */
    P0.H = payload;
    P0.L = payload;
    FSK 10
    P5 = [SP++];
    RTS;

    .align 4
payload:
    .long 0x00000005
    .long 0x00812340, 0x00000000
    .long 0x00000000, 0x00000000
    .long 0x02C00004, 0xDEADBEEF
    .long 0x00000000, 0x00000000
    .long 1234
