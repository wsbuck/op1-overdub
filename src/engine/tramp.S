/* Trampoline: TE's iter render slot (vtable +0x14) -> portable C engine_render().
 *
 * TE calls  render(this R0, int16* out R1, const int16* params R2)  from the encrypted L1 voice loop.
 * GCC code may use hardware loop 1 and assumes L0-L3 = 0; neither is proven safe at this call site,
 * so we save/restore both loop register sets and L0-L3, and zero L0-L3 for the C code.
 * C gets  engine_render(state = this + 4, out, params, pitch = _pitch_buf)  (4th arg on the stack).
 * this+0 is TE's vptr and must never be written; this+4 .. this+0x23 (32 B) belong to the engine.
 * Callee-saved R4-R7/P3-P5/FP are preserved by the C code itself (GCC ABI).
 * Profiling: CYCLES around the call; _prof_stats (uint32[3]: max, sum, calls) accumulates per render call
 * and the screen (src/slot/ui.c) reads and resets it each frame. Device builds put it in FREE region 0.
 * Live knobs: params (after LFO modulation) are copied to _prof_stats[3..4] (int16[4]) and [5] counts
 * render calls, so the screen can show what is heard (TE's DSP->GUI mailbox did not deliver; LOG.md).
 */
    .section .text.entry,"ax",@progbits
    .global _render
_render:
    [--SP] = RETS;
    [--SP] = LC0;
    [--SP] = LT0;
    [--SP] = LB0;
    [--SP] = LC1;
    [--SP] = LT1;
    [--SP] = LB1;
    [--SP] = L0;
    [--SP] = L1;
    [--SP] = L2;
    [--SP] = L3;
    L0 = 0;
    L1 = 0;
    L2 = 0;
    L3 = 0;
    P0.H = _prof_stats;         /* live knobs: copy params[0..3] (R2) before the call */
    P0.L = _prof_stats;
    P1 = R2;                    /* int16 array, may be only 2-byte aligned: halfword accesses */
    R3 = W[P1] (Z);
    W[P0 + 12] = R3;
    R3 = W[P1 + 2] (Z);
    W[P0 + 14] = R3;
    R3 = W[P1 + 4] (Z);
    W[P0 + 16] = R3;
    R3 = W[P1 + 6] (Z);
    W[P0 + 18] = R3;
    R3 = [P0 + 20];
    R3 += 1;
    [P0 + 20] = R3;
    R3 = CYCLES;
    [--SP] = R3;                /* start time */
    R0 += 4;                    /* skip vptr */
    P0.H = _pitch_buf;
    P0.L = _pitch_buf;
    SP += -16;                  /* 12 B arg area + 4th arg */
    [SP + 12] = P0;
    CALL _engine_render;
    SP += 16;
    R3 = [SP++];
    R0 = CYCLES;
    R0 = R0 - R3;               /* cycles for this call */
    P0.H = _prof_stats;
    P0.L = _prof_stats;
    R1 = [P0];
    R1 = MAX (R1, R0);
    [P0] = R1;
    R1 = [P0 + 4];
    R1 = R1 + R0;
    [P0 + 4] = R1;
    R1 = [P0 + 8];
    R1 += 1;
    [P0 + 8] = R1;
    L3 = [SP++];
    L2 = [SP++];
    L1 = [SP++];
    L0 = [SP++];
    LB1 = [SP++];
    LT1 = [SP++];
    LC1 = [SP++];
    LB0 = [SP++];
    LT0 = [SP++];
    LC0 = [SP++];
    RETS = [SP++];
    RTS;
