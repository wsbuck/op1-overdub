/* space FX screen: lunar ridges, a radio telescope, Earth and a rover's track (design/fx/space.svg by William Buck,
 * from design/fx/gen_space_moon.py, published as content/display/space.svg; its header comment is the runtime
 * contract, followed here). Included by ui.c after its SVG helpers and ui_repeat.h. Inputs: the knobs the FX used
 * (fx_live: TE's LFO moves them), the DSP's published state (space_ui: the TAIL zone played, the tail level, the duck
 * now, FREEZE / INFINITE, the green push, a clean start) and the frame counter (30 fps). Knob maps: space.h, shared
 * with the DSP.
 *
 *   blue   SPACE  the ridges for stage round(blue / 8192): ROOM ridge0, HALL ridge1, CHURCH ridge1 + 2, CATHEDRAL
 *                 ridge2 + 3, VOID `void`; between stages the shown ridges move down 6 px x frac(blue / 8192)
 *   green  DECAY  print1 .. print12: the first 1 + round(11 x the knob); `inf` while INFINITE holds (> 0.5), `flag`
 *                 while FREEZE holds (> 0.5) or the green push is held
 *   white  TAIL   TONE: phase round(5 x the zone amount) (0: a dark new Earth .. 5 full); SHIMMER: sh1 .. 9 (and in
 *                 +12+19 sx1 .. 8) by the shimmer amount heard (40 .. 100 %, faded in over the zone's first detent);
 *                 `halo` scaled 0.85 + 0.35 x the tail level (-72 .. 0 dBFS -> 0 .. 1) about Earth (262, 40);
 *                 SUB -12, GATE, SWELL: their group (`sub` an arc below Earth, `gate` a bar, `swell` a wedge; one
 *                 element each, so not amount-driven) in their zone
 *   orange MIX    wave1 .. 6: the first round(6 x wet); on the master `waves` scaled 1 - 0.55 x the duck now (0 dB ..
 *                 -24 dB -> 0 .. 1) about the feed (76.14, 86.86), duckdepth1 .. 4 above the middle (one per 4096);
 *                 as a synth / drum FX the hut goes dark (dry_ghost) once the dry left is under half
 *   FX off: the ridges in darkblue (ridges_dim), no track, flag, phase, halo, shimmer, waves or duck dots. A clean
 *   start (the lines clearing): no track, inf or shimmer.
 * Knob turns (stored knobs, >= 512 raw) show the value ~1.5 s in the design's top-left box in the knob's colour.
 * Diagnostic builds (--debug-ui): SHIFT held shows the CPU page instead (both instances' process call, this context's
 * FDN share, TE's records); normal builds: SHIFT does nothing here. Without space.svg (or one without the marker
 * `ridges_dim`): the line screen, with the CPU lines at its foot in diagnostic builds. */

extern struct space_ui space_ui[2] __attribute__((weak));
extern struct fx_pub space_pub[2] __attribute__((weak));

#if defined(UI_DEBUG) || defined(SP_CPU_PAGE)
#define SP_CPU 1                                    /* the CPU readings (--debug-ui probe images only) */
#endif

#ifdef __bfin__
#define TE_CYC(addr) (*(volatile const uint32_t *)(addr))   /* TE's CYCLES records (notes/fx-slot.md "CPU") */
#else
#define TE_CYC(addr) 0u
#endif

#define SP_SHOW 45                                  /* frames a readout stays (~1.5 s) */

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

/* the v1.1 zones' values: GATE's hold "200MS", SWELL's rise "0.47S" / "1.2S" (sep: between the number and the unit) */
static char *sp_put_zone_val(char *p, int z, int32_t amt, const char *sep)
{
    if (z == SP_Z_GATE)
        return put_str(put_str(put_dec(p, (uint32_t)(sp_gate_hold_ms(amt) + 5) / 10 * 10), sep), "MS");
    uint32_t ms = (uint32_t)sp_swell_ms(amt);
    if (ms >= 995) {                                                /* one decimal from 1 s */
        uint32_t t = (ms + 50) / 100;
        p = put_dec(p, t / 10);
        *p++ = '.';
        p = put_dec(p, t % 10);
    } else {                                                        /* two below: 0.15 .. 0.99 */
        uint32_t c = (ms + 5) / 10;
        p = put_str(p, "0.");
        if (c < 10)
            *p++ = '0';
        p = put_dec(p, c);
    }
    return put_str(put_str(p, sep), "S");
}

static char *sp_put_dec1(char *p, uint32_t v10)              /* tenths -> "2.4" (whole above 99) */
{
    if (v10 >= 1000)
        return put_dec(p, (v10 + 5) / 10);
    p = put_dec(p, v10 / 10);
    *p++ = '.';
    return put_dec(p, v10 % 10);
}

/* num / den rounded half to even (as the design's Python round()) */
static int sp_round(uint32_t num, uint32_t den)
{
    uint32_t q = udiv(num, den), rem = num - q * den;
    return (int)(q + (2 * rem > den || (2 * rem == den && (q & 1))));
}

/* the decay played (ms): the DSP's, else the knobs' */
static uint32_t sp_t60_ms(const struct space_ui *u, int32_t ks, int32_t kd)
{
    return u ? (uint32_t)u->t60_ms : ((sp_exp2_ui(sp_l2t60(ks) + sp_decay_l2(kd)) >> 4) * 1000u) >> 12;
}

/* "x1.00": DECAY's multiplier, two decimals */
static char *sp_put_mul(char *p, int32_t kd)
{
    uint32_t x100 = (sp_exp2_ui(sp_decay_l2(kd)) * 100u + 32768) >> 16;
    p = put_dec(put_str(p, "X"), x100 / 100);
    *p++ = '.';
    if (x100 % 100 < 10)
        *p++ = '0';
    return put_dec(p, x100 % 100);
}

/* The CPU readings: the master's and the synth / drum instance's process call (max / mean over 256 calls, the peak
 * since the FX started clean), this context's FDN share and TE's FX record, TE's callback record (last / max). Four
 * lines from y0, `dy` apart. */
#ifdef SP_CPU
static char *sp_put_kc(char *p, uint32_t c)                 /* cycles -> "41K" */
{
    return put_str(put_dec(p, (c + 512) >> 10), "K");
}

static void sp_cpu_lines(void *r, const struct space_ui *u, fp32 x, fp32 y0, fp32 dy, fp32 step)
{
    char buf[64], *p;
    for (int i = 1; i >= 0; i--) {
        const struct space_ui *v = space_ui ? &space_ui[i] : 0;
        p = put_str(buf, i ? "CPU MASTER " : "CPU SYNTH  ");
        if (v && v->blocks) {
            p = sp_put_kc(put_str(sp_put_kc(p, v->cyc_max), "/"), v->cyc_mean);
            p = sp_put_kc(put_str(p, " PEAK "), v->cyc_peak);
            put_str(p, v->half ? "  22KHZ" : "  44KHZ");
        } else
            put_str(p, "-");
        text_fast(r, buf, x, y0 + (i ? 0 : dy), step, GREY);
    }
    p = sp_put_kc(put_str(buf, "FDN "), u ? u->cyc_fdn : 0);
    sp_put_kc(put_str(sp_put_kc(put_str(p, "  TE FX "), TE_CYC(0x01BA26FC)), "/"), TE_CYC(0x01BA2704));
    text_fast(r, buf, x, y0 + 2 * dy, step, GREY);
    sp_put_kc(put_str(sp_put_kc(put_str(buf, "CALLBACK "), TE_CYC(0x01BA2620)), "/"), TE_CYC(0x01BA2628));
    text_fast(r, buf, x, y0 + 3 * dy, step, GREY);
}

/* SHIFT held on the space screen (diagnostic builds): the CPU page, larger, and the DSP's state under it. */
static void sp_cpu_page(void *r, const struct space_ui *u)
{
    char buf[64], *p;
    title(r, "SPACE CPU", WHITE);
    sp_cpu_lines(r, u, FP(8), FP(34), FP(16), FP(3) >> 1);
    if (!u) {
        text_fast(r, "FX OFF", FP(8), FP(104), FP(3) >> 1, GREY);
        return;
    }
    p = put_str(sp_put_dec1(put_str(buf, "T60 "), (uint32_t)(u->t60_ms + 50) / 100), "S  LEVEL ");
    p = put_db(p, dbfs_q8(u->level));
    put_db(put_str(p, "  DUCK "), dbfs_q8(u->duck));
    text_fast(r, buf, FP(8), FP(104), FP(3) >> 1, LIGHTBLUE);
    p = put_str(put_str(buf, sp_zone_names[u->zone < SP_ZONES ? u->zone : 0]), "  FRZ ");
    p = sp_put_pct(p, u->frz);
    p = sp_put_pct(put_str(p, " INF "), u->inf);
    put_str(p, u->clearing ? " CLEARING" : u->key ? " KEY" : "");
    text_fast(r, buf, FP(8), FP(120), FP(3) >> 1, LIGHTBLUE);
}
#endif

/* The line screen (no space.svg; notes/fx-reverb.md §11): the title and the four values, the state box, the level
 * meter and the duck now; the CPU lines at the foot in diagnostic builds. */
static void draw_space_lines(void *r, const int16_t *k, const struct space_ui *u, int stereo)
{
    char buf[48], *p;
    int32_t ks = knob_pos(k[0]), kd = knob_pos(k[1]), kt = knob_pos(k[2]), km = knob_pos(k[3]);

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
        p = sp_put_mul(put_str(sp_put_dec1(buf, (sp_t60_ms(u, ks, kd) + 50) / 100), " S  "), kd);
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
    } else if (z == SP_Z_SH12 || z == SP_Z_SH19 || z == SP_Z_SUB)
        sp_put_pct(put_str(p, " "), sp_shimmer_level(kt, z));
    else                                                            /* GATE HOLD 200 MS, SWELL RISE 0.47 S */
        sp_put_zone_val(put_str(p, z == SP_Z_GATE ? " HOLD " : " RISE "), z, sp_zone_amount(kt, z), " ");
    text(r, buf, xv, y, step, WHITE);

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
#ifdef SP_CPU
    sp_cpu_lines(r, u, x0, FP(108), FP(8), FP(1));
#endif
}

/* ---- the SVG screen ---- */
enum { SN_RIDGES, SN_RIDGES_DIM, SN_VOID, SN_VOID_DIM, SN_PRINTS, SN_INF, SN_FLAG, SN_HALO, SN_SHIM12, SN_SHIM1219,
       SN_SUB, SN_GATE, SN_SWELL, SN_WAVES, SN_DUCKDEPTH, SN_DRY, SN_DRYGHOST, SN_SINGLE };
static const char *const sp_single[SN_SINGLE] = { "ridges", "ridges_dim", "void", "void_dim", "prints", "inf", "flag",
    "halo", "shim12", "shim1219", "sub", "gate", "swell", "waves", "duckdepth", "dry", "dry_ghost" };
static const uint8_t sp_stage_ridges[5] = { 1, 2, 6, 12, 0 };    /* the ridges each stage shows (bit k: ridge k) */

static struct svgdoc space_doc RAM = { 0, 0 };
static void *sp_node[SN_SINGLE] RAM = { 0 };
static void *sp_ridge[4] RAM = { 0 }, *sp_ridge_dim[4] RAM = { 0 }, *sp_print[12] RAM = { 0 }, *sp_phase[5] RAM = { 0 };
static void *sp_sh[9] RAM = { 0 }, *sp_sx[8] RAM = { 0 }, *sp_wave[6] RAM = { 0 }, *sp_dd[4] RAM = { 0 };
struct sp_view {
    uint32_t frame;
    int32_t ctx, shown;                             /* the readout: screen context, knob shown, until frame */
    uint32_t until;
    int16_t ref[4];
};
static struct sp_view spv RAM = { 0, -1, -1, 0, { 0, 0, 0, 0 } };

/* pre + n (0 .. 99) + suf into buf, no libc */
static const char *sp_id(char *buf, const char *pre, int n, const char *suf)
{
    char *p = buf;
    while (*pre)
        *p++ = *pre++;
    int tens = 0;
    while (n >= 10)                                 /* no division: there is no libgcc */
        n -= 10, tens++;
    if (tens)
        *p++ = (char)('0' + tens);
    *p++ = (char)('0' + n);
    while (*suf)
        *p++ = *suf++;
    *p = 0;
    return buf;
}

static void sp_find(void **dst, const char *pre, int first, int n, const char *suf)
{
    char id[24];
    for (int i = 0; i < n; i++)
        dst[i] = te_node_find(space_doc.doc, sp_id(id, pre, first + i, suf), 0);
}

static int space_ready(void)
{
    if (space_doc.state)
        return space_doc.state > 0;
    if (!svg_open(&space_doc, "/yaffs2/content/display/space.svg", "space.svg"))
        return 0;
    if (!te_node_find(space_doc.doc, "ridges_dim", 0)) {           /* not this design's file: the line screen */
        space_doc.state = -1;
        return 0;
    }
    for (int i = 0; i < SN_SINGLE; i++)
        sp_node[i] = te_node_find(space_doc.doc, sp_single[i], 0);
    sp_find(sp_ridge, "ridge", 0, 4, "");
    sp_find(sp_ridge_dim, "ridge", 0, 4, "_dim");
    sp_find(sp_print, "print", 1, 12, "");
    sp_find(sp_phase, "phase", 1, 5, "");
    sp_find(sp_sh, "sh", 1, 9, "");
    sp_find(sp_sx, "sx", 1, 8, "");
    sp_find(sp_wave, "wave", 1, 6, "");
    sp_find(sp_dd, "duckdepth", 1, 4, "");
    return 1;
}

/* Uniform scale s (Q10) about (cx, cy) (Q10 units). */
static void sp_scale(void *node, int32_t s, int32_t cx, int32_t cy)
{
    set_matrix(node, s, 0, 0, s, cx - ((s * cx) >> 10), cy - ((s * cy) >> 10));
}

/* The value of knob `knob` (0..3) for the readout (12 characters at most: the box is 150 px), as heard. */
static void sp_readout(char *p, int knob, const int16_t *k, const struct space_ui *u, int stereo)
{
    int32_t ks = knob_pos(k[0]), kd = knob_pos(k[1]), kt = knob_pos(k[2]), km = knob_pos(k[3]);
    if (knob == 0)
        put_str(p, sp_space_names[sp_space_nearest(ks)]);
    else if (knob == 1) {
        int hold = sp_hold(kd);
        if (hold)
            put_str(p, hold == SP_HOLD_FREEZE ? "FREEZE" : "INFINITE");
        else
            sp_put_mul(put_str(sp_put_dec1(p, (sp_t60_ms(u, ks, kd) + 50) / 100), "S "), kd);  /* 3.8S X1.00 */
    } else if (knob == 2) {
        int z = u ? u->zone : sp_zone(kt);
        if (z == SP_Z_TONE) {
            int32_t x = sp_tone(kt);
            if (x > -1640 && x < 1640)
                put_str(p, "NATURAL");
            else
                sp_put_pct(put_str(p, x < 0 ? "DARK " : "BRIGHT "), x < 0 ? -x : x);
        } else if (z == SP_Z_SH12 || z == SP_Z_SH19 || z == SP_Z_SUB)
            sp_put_pct(put_str(p, z == SP_Z_SH12 ? "SHIM+12 " : z == SP_Z_SH19 ? "SH12+19 " : "SUB-12 "),
                       sp_shimmer_level(kt, z));
        else                                                        /* GATE 200MS, SWELL 0.47S */
            sp_put_zone_val(put_str(p, z == SP_Z_GATE ? "GATE " : "SWELL "), z, sp_zone_amount(kt, z), "");
    } else {
        int32_t wet, dry, ddb;
        sp_mix(km, stereo, &wet, &dry, &ddb);
        if (km <= 16384)
            sp_put_pct(put_str(p, "WET "), wet > 32767 ? 32767 : wet);
        else if (stereo)
            put_db(put_str(p, "DUCK "), -ddb);                      /* DUCK -18DB */
        else
            sp_put_pct(put_str(p, "DRY "), dry);                    /* all wet at the top */
    }
}

static void draw_space(void *r, const int16_t *k, const int16_t *stored, int running, int stereo, int mode, int ctx)
{
    const struct space_ui *u = running && space_ui ? &space_ui[stereo] : 0;
    uint32_t fr = ++spv.frame;
    if (&rep_key) {                                                 /* the green push's heartbeat (repeat.h) */
        rep_key.screen_ctl = (uint8_t)(mode == 3 ? RK_MASTER : mode == 1 ? RK_DRUM : RK_SYNTH);
        rep_key.screen_seen = &te_blocks ? te_blocks : 0;
    }
    /* the readout: a stored knob moved >= 512 raw shows its value ~1.5 s (not on entering the screen) */
    if (ctx != spv.ctx) {
        spv.ctx = ctx;
        spv.until = fr;
        for (int i = 0; i < 4; i++)
            spv.ref[i] = stored[i];
    }
    for (int i = 0; i < 4; i++) {
        int32_t d = stored[i] - spv.ref[i];
        if (d >= 512 || d <= -512) {
            spv.ref[i] = stored[i];
            spv.shown = i;
            spv.until = fr + SP_SHOW;
        }
    }
#ifdef SP_CPU
    if (WH_SHIFT) {
        sp_cpu_page(r, u);
        return;
    }
#endif
    if (!space_ready()) {
        draw_space_lines(r, k, u, stereo);
        return;
    }
    int off = !u, clean = u && u->clearing;
    int32_t ks = knob_pos(k[0]), kd = knob_pos(k[1]), kt = knob_pos(k[2]), km = knob_pos(k[3]);

    /* blue: the ridges of stage round(SPACE), moved down toward the next by 6 px x frac; darkblue when off */
    int stage = sp_round((uint32_t)ks, 8192);
    stage = stage > 4 ? 4 : stage;
    int32_t frac = ks & 8191;
    fp32 down = stage < 4 && frac > 81 ? (frac * 6) >> 3 : 0;      /* 6 px x frac, Q10 */
    for (int i = 0; i < 4; i++) {
        int on = sp_stage_ridges[stage] >> i & 1;
        show(sp_ridge[i], on);
        show(sp_ridge_dim[i], on);
        if (on) {
            translate(sp_ridge[i], 0, down);
            translate(sp_ridge_dim[i], 0, down);
        }
    }
    show(sp_node[SN_VOID], stage == 4);
    show(sp_node[SN_VOID_DIM], stage == 4);
    show(sp_node[SN_RIDGES], !off);
    show(sp_node[SN_RIDGES_DIM], off);

    /* green: the track (the knob), INFINITE's loop, FREEZE's flag (or the push held) */
    int n = 1 + sp_round(11u * (uint32_t)kd, 32767);
    for (int i = 0; i < 12; i++)
        show(sp_print[i], i < n);
    show(sp_node[SN_PRINTS], !off && !clean);
    show(sp_node[SN_INF], !off && !clean && u->inf > 16384);
    show(sp_node[SN_FLAG], !off && (u->frz > 16384 || u->key));

    /* white: Earth's phase (TONE), the halo with the tail level, the shimmer pillar */
    int z = u ? u->zone : sp_zone(kt);
    int ph = z == SP_Z_TONE ? sp_round(5u * (uint32_t)sp_zone_amount(kt, z), 32767) : 0;
    for (int i = 0; i < 5; i++)
        show(sp_phase[i], !off && i + 1 == ph);
    show(sp_node[SN_HALO], !off);
    if (u) {
        int32_t db = dbfs_q8(u->level) + 72 * 256;                  /* Q8 dB above -72 */
        int32_t lv = db <= 0 ? 0 : db >= 72 * 256 ? 32768 : (int32_t)udiv((uint32_t)db << 15, 72 * 256);
        sp_scale(sp_node[SN_HALO], (27853 + ((11469 * lv) >> 15)) >> 5, FP(262), FP(40));   /* 0.85 + 0.35 x */
    }
    int shim = z == SP_Z_SH12 || z == SP_Z_SH19;
    int32_t sa = shim ? sp_shimmer_level(kt, z) : 0;
    int nsh = sp_round(9u * (uint32_t)sa, 32767), nsx = sp_round(8u * (uint32_t)sa, 32767);
    for (int i = 0; i < 9; i++)
        show(sp_sh[i], i < nsh);
    for (int i = 0; i < 8; i++)
        show(sp_sx[i], i < nsx);
    show(sp_node[SN_SHIM12], !off && !clean && shim);
    show(sp_node[SN_SHIM1219], !off && !clean && z == SP_Z_SH19);
    show(sp_node[SN_SUB], !off && z == SP_Z_SUB);                  /* one element each: shown in the zone */
    show(sp_node[SN_GATE], !off && z == SP_Z_GATE);
    show(sp_node[SN_SWELL], !off && z == SP_Z_SWELL);

    /* orange: the waves (wet), shrunk by the duck now and the duck depth's dots (master); the hut (dry) */
    int32_t wet, dry, ddb;
    sp_mix(km, stereo, &wet, &dry, &ddb);
    int nw = sp_round(6u * (uint32_t)wet, 32768);
    for (int i = 0; i < 6; i++)
        show(sp_wave[i], i < nw);
    show(sp_node[SN_WAVES], !off);
    int32_t duck = 0;                                               /* Q15, 0 dB .. -24 dB */
    if (u && stereo && u->duck < 32768) {
        int32_t db = -dbfs_q8(u->duck);
        duck = db <= 0 ? 0 : db >= 24 * 256 ? 32768 : (int32_t)udiv((uint32_t)db << 15, 24 * 256);
    }
    sp_scale(sp_node[SN_WAVES], 1024 - ((563 * duck) >> 15), FP(76) + 143, FP(86) + 881);    /* (76.14, 86.86) */
    int nd = stereo && km > 16384 ? (km - 16384 + 4095) >> 12 : 0;
    for (int i = 0; i < 4; i++)
        show(sp_dd[i], i < nd);
    show(sp_node[SN_DUCKDEPTH], !off);
    int ghost = !stereo && dry < 16384;
    show(sp_node[SN_DRY], !ghost);
    show(sp_node[SN_DRYGHOST], ghost);

    svg_render(space_doc.doc);
    if (fr < spv.until && spv.shown >= 0) {                         /* the readout in the design's top-left box */
        static const int colours[4] = { BLUE, GREEN, WHITE, ORANGE };
        char buf[24];
        sp_readout(buf, spv.shown, k, u, stereo);
        text(r, buf, FP(8), FP(7), FP(2), colours[spv.shown]);
    }
    svg_cost(r);
}
