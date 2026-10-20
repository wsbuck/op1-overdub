/* The space FX's screen (a placeholder line screen until a designed one; notes/fx-reverb.md §11): included by ui.c
 * after its drawing primitives (line_fp, line_fast, text, text_fast, put_*, dbfs_q8, udiv, the colour enum, RAM).
 * Knob maps: space.h, shared with the DSP. Colours: the knob colours for knob values only (blue SPACE, green DECAY,
 * white TAIL, orange MIX); labels grey; states (FROZEN, the meter, the duck now) light blue; diagnostics grey.
 *
 *   SPACE   the space's name (the title) and the morph:            CHURCH > CATHEDRAL 40%
 *   DECAY   the decay played (mid band) and the multiplier:        3.8 S  X1.0   | INFINITE | FREEZE
 *   TAIL    the zone and the amount in it:                         TONE DARK 60% | SHIMMER +12 70% | SUB -12 V1.1
 *   MIX     the wet; above the middle, on the master the duck depth, on synth / drum the dry left
 *   the state: FROZEN (the DECAY detent or the held green push, KEY), INFINITE, CLEARING (a clean start)
 *   the tail level meter (light blue, -72 .. 0 dBFS) and the duck now (master)
 *   CPU: both instances' process call over the last 256 calls (max / mean, the peak since the FX started), this
 *   context's FDN share; TE's records for the callback and the synth LFO + FX stage (last / max).
 * Drawn from the knobs the FX used (fx_live: TE's LFO moves them on synth / drum) and the DSP's state while it runs. */

extern struct space_ui space_ui[2] __attribute__((weak));
extern struct fx_pub space_pub[2] __attribute__((weak));

#ifdef __bfin__
#define TE_CYC(addr) (*(volatile const uint32_t *)(addr))   /* TE's CYCLES records (notes/fx-slot.md "CPU") */
#else
#define TE_CYC(addr) 0u
#endif

/* 2^(y / 65536), Q16, for y in -16 .. +14 octaves */
static uint32_t sp_exp2_ui(int32_t y)
{
    int32_t n = y >> 16;
    if (n < -16)
        return 0;
    uint32_t m = exp2_q16_ui(y & 0xFFFF);                           /* 2^frac, Q16 */
    return n >= 0 ? m << n : m >> -n;
}

static char *sp_put_pct(char *p, int32_t q15)                /* Q15 -> "40%" */
{
    p = put_dec(p, (uint32_t)((q15 < 0 ? 0 : q15) * 100 + 16384) >> 15);
    return put_str(p, "%");
}

static char *sp_put_dec1(char *p, uint32_t v10)              /* tenths -> "2.4" (whole above 99) */
{
    if (v10 >= 1000)
        return put_dec(p, (v10 + 5) / 10);
    p = put_dec(p, v10 / 10);
    *p++ = '.';
    return put_dec(p, v10 % 10);
}

static char *sp_put_kc(char *p, uint32_t c)                 /* cycles -> "41K" */
{
    return put_str(put_dec(p, (c + 512) >> 10), "K");
}

static void draw_space(void *r, const int16_t *k, int running, int stereo, int mode)
{
    const struct space_ui *u = running && space_ui ? &space_ui[stereo] : 0;
    char buf[48], *p;
    int32_t ks = knob_pos(k[0]), kd = knob_pos(k[1]), kt = knob_pos(k[2]), km = knob_pos(k[3]);
    if (&rep_key) {                                                 /* the green push's heartbeat (repeat.h) */
        rep_key.screen_ctl = (uint8_t)(mode == 3 ? RK_MASTER : mode == 1 ? RK_DRUM : RK_SYNTH);
        rep_key.screen_seen = &te_blocks ? te_blocks : 0;
    }

    /* blue: the space, the title */
    int a = sp_space_nearest(ks);
    title(r, sp_space_names[a], BLUE);
    fp32 x0 = FP(8), xv = FP(62), dy = FP(17), step = FP(3) >> 1;
    fp32 y = FP(30);
    text_fast(r, "SPACE", x0, y + FP(2), FP(1), GREY);
    int32_t off = ks - (a == 4 ? 32767 : a * 8192);
    if (off > -512 && off < 512) {                                  /* at an anchor: its own RT60 (DECAY's noon) */
        p = put_str(put_str(buf, sp_space_names[a]), "  OWN ");
        put_str(sp_put_dec1(p, ((((sp_exp2_ui(sp_l2t60(ks)) >> 4) * 1000u) >> 12) + 50) / 100), " S");
    } else {                                                          /* between two anchors: from > to, how far */
        int lo = off < 0 ? a - 1 : a, hi = lo + 1;
        int32_t span = hi == 4 ? 8191 : 8192, f = ks - lo * 8192;
        p = put_str(put_str(put_str(buf, sp_space_names[lo]), " > "), sp_space_names[hi]);
        *p++ = ' ';
        sp_put_pct(p, (int32_t)udiv((uint32_t)f << 15, (uint32_t)span));
    }
    text(r, buf, xv, y, step, BLUE);

    /* green: the decay */
    y += dy;
    text_fast(r, "DECAY", x0, y + FP(2), FP(1), GREY);
    int hold = sp_hold(kd);
    if (hold == SP_HOLD_FREEZE)
        put_str(buf, "FREEZE  IN MUTED");
    else if (hold == SP_HOLD_INFINITE)
        put_str(buf, "INFINITE  IN OPEN");
    else {
        int32_t l2m = sp_decay_l2(kd);
        uint32_t ms = u ? (uint32_t)u->t60_ms : ((sp_exp2_ui(sp_l2t60(ks) + l2m) >> 4) * 1000u) >> 12;
        p = sp_put_dec1(buf, (ms + 50) / 100);
        p = put_str(p, " S  X");
        uint32_t x100 = (sp_exp2_ui(l2m) * 100u + 32768) >> 16;    /* the multiplier, hundredths */
        p = put_dec(p, x100 / 100);
        *p++ = '.';
        if (x100 % 100 < 10)
            *p++ = '0';
        p = put_dec(p, x100 % 100);
        if (kd >= 28 << 10)                                         /* the holds are two detents up */
            put_str(p, " > INF");
    }
    text(r, buf, xv, y, step, GREEN);

    /* white: the tail effect */
    y += dy;
    text_fast(r, "TAIL", x0, y + FP(2), FP(1), GREY);
    int z = u ? u->zone : sp_zone(kt);
    p = put_str(buf, sp_zone_names[z]);
    if (z == SP_Z_TONE) {
        int32_t x = sp_tone(kt);
        if (x > -1640 && x < 1640)
            put_str(p, " NATURAL");
        else
            sp_put_pct(put_str(p, x < 0 ? " DARK " : " BRIGHT "), x < 0 ? -x : x);
    } else if (z == SP_Z_SH12 || z == SP_Z_SH19)
        sp_put_pct(put_str(p, " "), sp_shimmer_level(kt, z));
    else
        put_str(p, " SOON");                                        /* reserved: nothing yet (natural tone) */
    text(r, buf, xv, y, step, z >= SP_Z_SUB ? GREY : WHITE);

    /* orange: the mix */
    y += dy;
    text_fast(r, "MIX", x0, y + FP(2), FP(1), GREY);
    int32_t wet, dry, ddb;
    sp_mix(km, stereo, &wet, &dry, &ddb);
    p = put_dec(put_str(buf, "DRY "), (uint32_t)(dry * 100 + 16384) >> 15);   /* percent, no sign: the row's tight */
    p = put_dec(put_str(p, " WET "), (uint32_t)(wet * 100 + 16384) >> 15);
    if (stereo && ddb)
        put_db(put_str(p, " DUCK "), -ddb);
    text(r, buf, xv, y, step, ORANGE);

    /* the state: FROZEN / INFINITE / CLEARING (a box, top right) */
    const char *state = 0;
    int sc = LIGHTBLUE;
    if (u && u->clearing) {
        state = "CLEARING";
        sc = GREY;
    } else if (u ? u->frz > 16384 : hold == SP_HOLD_FREEZE)
        state = u && u->key ? "FREEZE HELD" : "FROZEN";
    else if (u ? u->inf > 16384 : hold == SP_HOLD_INFINITE)
        state = "INFINITE";
    if (state) {
        int len = 0;
        while (state[len])
            len++;
        fp32 bx1 = FP(312), bx0 = bx1 - FP(9 * len + 8);
        line_fp(r, bx0, FP(5), bx1, FP(5), FP(1), sc);
        line_fp(r, bx1, FP(5), bx1, FP(21), FP(1), sc);
        line_fp(r, bx1, FP(21), bx0, FP(21), FP(1), sc);
        line_fp(r, bx0, FP(21), bx0, FP(5), FP(1), sc);
        text(r, state, bx0 + FP(4), FP(9), step, sc);
    }

    /* the tail level, -72 .. 0 dBFS over 200 units, and the duck now (master) */
    y = FP(100);
    text_fast(r, "LEVEL", x0, y - FP(3), FP(1), GREY);
    line_fast(r, xv, y, xv + FP(200), y, GREY);
    for (int i = 0; i <= 6; i++)                                    /* ticks every 12 dB */
        vline_fast(r, xv + (fp32)udiv(FP(200) * (uint32_t)i, 6), y - FP(2), y + FP(2), GREY);
    text_fast(r, "0DB", xv + FP(196), y + FP(4), FP(1), GREY);
    if (u && u->level > 0) {
        int32_t db = dbfs_q8(u->level) + 72 * 256;                  /* Q8 dB above -72 */
        if (db > 0) {
            fp32 w = (fp32)(udiv((uint32_t)(db > 72 * 256 ? 72 * 256 : db) * 200u, 72 * 256) << 10);
            line_fp(r, xv, y, xv + w, y, FP(3), LIGHTBLUE);
        }
    }
    if (u && stereo && u->duck < 32700) {
        p = put_db(put_str(buf, "DUCK "), dbfs_q8(u->duck));
        text_fast(r, buf, xv + FP(208), y - FP(3), FP(1), LIGHTBLUE);
    }

    /* CPU: the master's and the synth / drum instance's process call (max / mean over 256 calls, the peak since the
     * FX started), this context's FDN share, TE's records (last / max) */
    for (int i = 1; i >= 0; i--) {
        const struct space_ui *v = space_ui ? &space_ui[i] : 0;
        p = put_str(buf, i ? "CPU MASTER " : "CPU SYNTH  ");
        if (v && v->blocks) {
            p = sp_put_kc(put_str(sp_put_kc(p, v->cyc_max), "/"), v->cyc_mean);
            p = sp_put_kc(put_str(p, " PEAK "), v->cyc_peak);
            put_str(p, v->half ? "  22KHZ" : "  44KHZ");
        } else
            put_str(p, "-");
        text_fast(r, buf, x0, FP(i ? 110 : 120), FP(1), GREY);
    }
    p = sp_put_kc(put_str(buf, "FDN "), u ? u->cyc_fdn : 0);
    p = sp_put_kc(put_str(sp_put_kc(put_str(p, "  TE FX "), TE_CYC(0x01BA26FC)), "/"), TE_CYC(0x01BA2704));
    sp_put_kc(put_str(sp_put_kc(put_str(p, "  CALLBACK "), TE_CYC(0x01BA2620)), "/"), TE_CYC(0x01BA2628));
    text_fast(r, buf, x0, FP(130), FP(1), GREY);
}
