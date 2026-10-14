/* Drum probe (drumprobe.c, a test drum engine) and its screen (ui.c): shared state and TE's drum-side addresses.
 * Research and route: notes/drum-engine.md. The TE arrays are linker symbols: fixed addresses on the device
 * (tools/build_slot.py), arrays in the simulator (test/drumprobe_main.c) and the host preview.
 */
#ifndef DRUMPROBE_H
#define DRUMPROBE_H
#include <stdint.h>

#define DP_VOICES 12                       /* kDrumVoiceCount */
#define DP_KEYS 24

extern volatile int32_t te_drum_key[];     /* 0xFF801334: key 0..23 per voice (VoiceCtl note-on) */
extern volatile int16_t te_drum_vel[];     /* 0xFF8012EC: velocity << 8 per voice (0x7F00 from the OP-1's keys) */
extern volatile int32_t te_drum_ratio[];   /* 0xFF801304: 2^(x/12) pitch ratio per voice (fixed point, unknown Q) */
extern volatile int32_t te_drum_edit;      /* 0xFF801044: the key the knobs edit (last pressed) */
extern int16_t te_drum_params[];           /* 0x01AAADB4: [8 slots][24 keys][8] per-key params */
extern int16_t ranges[];                   /* slot block: knob range table, 0x40 B (32 int16) per engine id */

/* What the engine saw, for the screen. Written by the audio thread (render) and the note-on caller. */
struct dp_diag {
    uint32_t noteons, noteoffs, renders, cyc_max;
    int32_t key, vel, voice, ratio;        /* last note-on */
    int32_t inits, row_copied;             /* factory calls (slot inits), range row 9 copied to ours */
    int32_t level[DP_VOICES];              /* per voice: amplitude, Q15 (0 = silent) */
    int16_t live[8];                       /* params of the newest sounding voice, as rendered (after the LFO) */
};
extern struct dp_diag drumprobe_diag;
extern uint32_t dp_type_hits[2];           /* TE's per-key loader [0] / saver [1] took the dbox path for our type */
extern const int32_t drum_id;              /* our drum engine id (slot_gen.inc) */

#endif
