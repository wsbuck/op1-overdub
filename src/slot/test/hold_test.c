/* Host test of src/slot/hold.c (HOLD and CHORD) with stand-ins for TE's note functions and state.
 *   cc -std=gnu99 -I src/slot src/slot/test/hold_test.c -o /tmp/hold_test && /tmp/hold_test */
#include <stdint.h>
#include <stdio.h>

static int32_t play_mode, slot, octave[8];
static uint8_t key_down[24], locked, shift_down;
static int offs[64], noffs, shift_calls, ons[64], nons, sounding[128], bad_range;

#define PLAY_MODE play_mode
#define SLOT slot
#define OCTAVE(s) octave[s]
#define KEY_DOWN key_down
#define VOICECTL_LOCKED locked
#define SHIFT_DOWN shift_down

void te_make_note(int32_t *n, int key, int vel) { n[0] = key + 0x35 + 12 * OCTAVE(SLOT); n[1] = vel; }
void te_note_off(int32_t *n)
{
    offs[noffs++] = n[0];
    if (n[0] >= 0 && n[0] < 128 && sounding[n[0]])
        sounding[n[0]]--;
}
void te_note_on(int32_t *n, int flags)
{
    (void)flags;
    if (n[0] < 0 || n[0] > 127) {
        bad_range++;                                   /* TE asserts on these */
        return;
    }
    ons[nons++] = n[0];
    sounding[n[0]]++;
}
int te_synth_shiftkey(void *self, int key) { (void)self; (void)key; shift_calls++; return 0; }

#define HOLD_HOST_TEST
#include "../hold_impl.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL " __VA_ARGS__); printf("\n"); fails++; } else { printf("PASS " __VA_ARGS__); printf("\n"); } } while (0)

static void key_dn(int key)
{
    int32_t n[5];
    key_down[key] = 1;
    te_make_note(n, key, 0x7F);
    chord_keydown(n);
}

static int count_sounding(void)
{
    int c = 0;
    for (int i = 0; i < 128; i++)
        c += sounding[i];
    return c;
}

static int is_sounding(int note) { return note >= 0 && note < 128 && sounding[note]; }

static void key_up(int key)
{
    int32_t n[5];
    key_down[key] = 0;
    te_make_note(n, key, 0x40);
    hold_keyup(n);
}

int main(void)
{
    octave[0] = 1;
    key_up(3);
    CHECK(noffs == 1 && offs[0] == 3 + 0x35 + 12, "hold off: key-up releases at once");
    noffs = 0;
    CHECK(hold_shiftkey(0, 7) == 1 && hold_on, "SHIFT+T2 turns hold on and is handled");
    key_up(0); key_up(4); key_up(7);
    CHECK(noffs == 0 && hold_keys == ((1u << 0) | (1u << 4) | (1u << 7)), "hold on: key-ups are recorded, not released");
    key_down[4] = 1;                                   /* key 4 pressed again and still down */
    octave[0] = 2;                                     /* octave changed while holding */
    hold_shiftkey(0, 7);
    CHECK(!hold_on && noffs == 2 && offs[0] == 0x35 + 24 && offs[1] == 7 + 0x35 + 24 && hold_keys == 0,
          "hold off: releases held keys not down, rebuilt at the current octave (like TE's key-up)");
    noffs = 0;
    hold_shiftkey(0, 7);
    play_mode = 1;
    key_up(5);
    CHECK(noffs == 1, "drum mode: key-ups pass through even with hold on");
    play_mode = 0; noffs = 0;
    key_up(6);
    locked = 1;
    hold_shiftkey(0, 7);
    CHECK(noffs == 0 && hold_keys == (1u << 6), "while VoiceCtl is locked: nothing released, keys kept");
    locked = 0;
    hold_shiftkey(0, 7);
    hold_shiftkey(0, 7);
    CHECK(noffs == 1 && offs[0] == 6 + 0x35 + 24, "toggling again releases them");
    CHECK(hold_shiftkey(0, 8) == 0 && shift_calls == 1, "other SHIFT keys go to TE's handler");

    /* CHORD */
    if (hold_on)
        hold_shiftkey(0, 7);                           /* hold off */
    for (int i = 0; i < 128; i++)
        sounding[i] = 0;
    octave[0] = 0;
    int C = 0x35;                                      /* key 0 at octave 0 */
    key_dn(0); key_up(0);
    CHECK(count_sounding() == 0 && !chord_on, "chord off: plain keys play single notes");
    shift_down = 1;
    key_dn(0); key_dn(4); key_dn(7);                   /* learn C E G */
    CHECK(chord_learning && chord_n == 3 && chord_iv[1] == 4 && chord_iv[2] == 7 && count_sounding() == 3,
          "SHIFT + C E G: learning a 3-note shape, learn notes sound");
    key_up(0); key_up(4); key_up(7);
    shift_down = 0;
    key_dn(5);                                         /* F */
    CHECK(!chord_learning && chord_on && count_sounding() == 3 && is_sounding(C + 5) && is_sounding(C + 9)
          && is_sounding(C + 12), "first plain key ends learning and plays the shape on it (F A C)");
    key_dn(9);                                         /* A: A C# E, shares A with F's chord */
    CHECK(count_sounding() == 5 && is_sounding(C + 13) && is_sounding(C + 16), "overlapping chord adds only new notes");
    key_up(5);
    CHECK(is_sounding(C + 9) && !is_sounding(C + 5) && !is_sounding(C + 12) && count_sounding() == 3,
          "releasing F keeps the shared A (reference counted)");
    octave[0] = 1;                                     /* octave change while A's chord sounds (TE transposes voices) */
    for (int i = 0; i < 128; i++)
        sounding[i] = 0;
    sounding[C + 12 + 9]++; sounding[C + 12 + 13]++; sounding[C + 12 + 16]++;
    key_up(9);
    CHECK(count_sounding() == 0, "octave change between down and up: the chord is released at the new octave");
    octave[0] = 0;
    shift_down = 1;
    key_dn(0); key_dn(16);                             /* root + major 10th (16 semitones) */
    key_up(0); key_up(16);
    shift_down = 0;
    key_dn(2);
    CHECK(chord_n == 2 && is_sounding(C + 2) && is_sounding(C + 18) && count_sounding() == 2,
          "SHIFT + C + E' learns root + major 10th; D plays D + F#'");
    key_up(2);
    octave[0] = 4;                                     /* top octave: the 10th would be note 0x35 + 48 + 23 + 16 > 127 */
    bad_range = 0;
    key_dn(23); key_up(23);
    CHECK(bad_range == 0 && count_sounding() == 0, "notes above 127 are skipped, nothing hangs");
    octave[0] = 0;
    hold_shiftkey(0, 7);                               /* hold + chord */
    key_dn(0); key_up(0); key_dn(7); key_up(7);
    CHECK(count_sounding() == 4, "hold on: released chords keep sounding");
    key_dn(0);
    CHECK(count_sounding() == 4 && nons > 0, "re-pressing a held chord key retriggers it, no extra voices");
    key_up(0);
    hold_shiftkey(0, 7);
    CHECK(count_sounding() == 0, "hold off: all held chords released");
    shift_down = 1;
    key_dn(3); key_up(3);
    shift_down = 0;
    key_dn(3);
    CHECK(!chord_on && count_sounding() == 1, "SHIFT + a single key turns chord mode off");
    key_up(3);
    shift_down = 1;
    key_dn(0); key_dn(4); key_dn(7); key_dn(11); key_dn(14);
    shift_down = 0;
    CHECK(chord_n == 4, "shapes are capped at 4 notes");
    for (int k = 0; k < 24; k++) key_up(k);
    key_dn(0);
    key_down[0] = 0;
    play_mode = 1;                                     /* switch to drum mode with a chord down: TE stops the voices */
    for (int i = 0; i < 128; i++)
        sounding[i] = 0;
    key_dn(1); key_up(1);
    play_mode = 0;
    key_dn(0);
    CHECK(count_sounding() == 4, "after a play-mode switch the chord state is reset and plays fully again");
    key_up(0);
    CHECK(count_sounding() == 0, "... and releases cleanly");
    /* A chord key held across a mode switch with no key event in the other mode: TE clears s_note_ons and
     * then never delivers its key-up (FUN_019ac2f8 returns early), so its references must not leak. */
    key_dn(0);                                         /* C chord (4-note shape) */
    play_mode = 1;
    for (int i = 0; i < 24; i++)
        key_down[i] = 0;                               /* s_note_ons cleared by the switch */
    for (int i = 0; i < 128; i++)
        sounding[i] = 0;                               /* voices stopped */
    play_mode = 0;                                     /* back, without any key event in drum mode */
    key_dn(4);                                         /* a chord sharing notes with C's */
    int shared_ok = 1;
    for (int i = 0; i < chord_n; i++)
        shared_ok &= is_sounding(C + 4 + chord_iv[i]);
    CHECK(shared_ok && count_sounding() == chord_n, "chord key lost in a mode switch: later chords keep every note");
    key_up(4);
    CHECK(count_sounding() == 0, "... and release cleanly");
    printf(fails ? "\n%d failed\n" : "\nall passed\n", fails);
    return fails != 0;
}
