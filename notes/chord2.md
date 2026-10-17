# CHORD v2 and the key-chord map (firmware 246): scope

Status: **MVP on the device (NIGHT2, LOG.md test 85: every step passes)**: §12 "MVP as built". **Phase 2 and 3 built
(NIGHT3, test 86)**: §13 "Phase 2 as built" (SMOOTH, STRUM, WRITE, bipolar VOICING, intervals, the played-notes view).
Phase 4 is still design only.
Mockups: `design/chord2/`:
- `chord2_ui.h`: the draw code;
- `chord2_model.h`: qualities, voicings, names, recognition, SMOOTH, fill;
- `chord2_factory.h`: the factory maps;
- `chord2_preview.c`: the host harness and the model checks;
- `chord2_bfin.c`: the Blackfin compile check;
- `render.py OUTDIR`: writes the PNGs plus `index.html`;
- `captions.py`.

The mockups are drawn by C written as device code (ui.c's primitives, no division, no libc), at the device scale.
Two subagents did the static probes for this scope; their decompiles are in the session scratchpad (`probe_keys/`,
`probe_save/`). Tags: **Verified** = read at the call site in the disassembly; **Confirmed** = consistent decompile
or TE usage; **Hypothesis** = inferred, untested.

## Summary
- **One feature, CHORD, one mode per synth slot:** OFF, SHAPE (CHORD1's parallel shape, now voiceable) or MAP 1..8
  (each key its own chord; unmapped keys play their note).
- **CHORD page (SHIFT + SPLIT):** the 24 keys labelled with their chords, the edited chord big.
  - The plain knobs edit the last key pressed: ROOT, TYPE, TOP (melody-note ladder), VOICING.
  - SHIFT + knobs set the map: MODE, STRUM, SMOOTH, BASS.
  - Learn a chord into a key by holding SHIFT and playing it. LIFT / DROP / SPLIT+key copy, paste and clear, and SHIFT+LIFT undoes.
  - SHIFT + DROP opens WRITE: scale fill, copy + transpose, factory, empty.
- **Playing:** our own voice budget, so TE never leaves chord fragments. HOLD latches chords only, and a new chord
  replaces the held one. SMOOTH voice-leads deterministically, and STRUM spreads onsets.
- **Storage:** our own A/B file on the internal disk. TE never reads it.
- **MVP:** MAP with the page, learn, the four chord knobs, clear/undo, factory maps, per-slot mode, voice budget,
  HOLD-chords, file save, badge plus TE-screen chip. It is gated by one probe image (CHORDPROBE).

## 1. CHORD today (CHORD1, shipped; device: "all works", LOG.md test images 34..38)
Code: `src/slot/hold_impl.h` (`chord_keydown`, `hold_keyup`, `hold_shiftkey`), wrappers in `slot.S`. Design notes:
notes/seq-chord.md "CHORD1 as built" and notes/hold.md. Feature bit `FEAT_CHORD` 2 in `_feature_mask` (`apply.py
--without chord` turns it off; SHIFT+notes then play stock).
- **Trigger:** CHORD1 replaces the keyboard's note-on CALL at `0x019AC290` (`FUN_019ac0b0` → `FUN_01a85750`).
  - `SHIFT_DOWN` (`0x01DD7F78`) set: learn mode. The first SHIFT+note clears the shape and becomes the root
    (`learn_root`). Each further one adds `note − root`, clamped ±35, ignoring duplicates, up to `CHORD_MAX` 4.
  - Learn notes sound as played.
  - The first plain note ends learning. `chord_on = chord_n >= 2`, so SHIFT + one note turns chord mode off.
- **Play:** each key plays `key + chord_iv[i]` for the stored shape. Notes are built by `te_make_note(rel, vel)`, so
  the octave keys transpose. Notes outside 0..127 are skipped (VoiceCtl asserts).
  - Per key, `key_n[24]` / `key_iv[24][4]` store the shape the key was played with.
  - Per relative note, `rel_ref[96]` holds a reference count, so overlapping chords don't cut shared notes.
- **Release:** through HOLD's key-up hook (`0x019AC3A6`). With HOLD on, the key is kept in `hold_keys`.
  - A held key pressed again is released first, so it retriggers.
  - A play-mode switch resets the bookkeeping. `purge_stale()` drops keys whose key-up can no longer come.
- **Screen:** a CHORD/LEARN box and a semitone ruler (`chord_badge`, ui.c), on our engines' screens only.
- **Limits:**
  - one shape for all keys, transposed in parallel;
  - no names or recognition;
  - no voicing edit: changing anything means learning again;
  - learning needs a plain note to end it, and that note already plays the chord;
  - nothing persists over power-off;
  - nothing shows on TE's screens;
  - sequencers and the arp take the keys first (no chords);
  - MIDI-in notes are not chorded;
  - a plain note's key-up can cut a chord note of the same pitch: today every key is a chord, so this is moot, but with
    a map it would not be.

## 2. Design

### Modes
- **Per synth slot** (1..8): OFF, SHAPE, MAP 1..8. A lead in slot 2 stays plain while slot 3 plays NEO SOUL
  (UX round 2 blocker).
  - Set on the page with **SHIFT + blue**. Off the page, SHIFT + blue stays TE's.
- **SHAPE is a map whose keys all hold the same chord**, so there is one code path. CHORD1's off-page gesture still
  works: SHIFT + notes learns the shape, and SHIFT + one note sets the slot to OFF. Mapped keys are never written by it.
- **Power-on: every slot OFF**, with its last mode remembered: one SHIFT+blue detent brings it back.
  - Once the TE-screen chip is proven on the device, power-on restores the modes instead (open question 3).

### The CHORD page
- **Open / close:** SHIFT + SPLIT (scissors, key 0xC). It is unused by TE on the synth screen (Verified). T1..T4 close
  the page and go on to their TE page. Opening the page changes nothing about what plays. Keys play as normal while
  it is open.
- **Plain knobs edit the chord of the edited key** (the OP-1's drum-key idiom):
  | knob | edits | shown as |
  |---|---|---|
  | blue | ROOT, semitones from the key | blue underline under the root in the big name |
  | green | TYPE, 28 qualities (below), or PLAYED for a learned chord | green underline under the quality |
  | white | **TOP**, a ladder: each detent puts the next chord tone on top, across octaves | white top mark on the ruler, top note "B4" |
  | orange | **VOICING**: close, drop 2, drop 3, spread | orange word under the ruler |
  - TOP replaced v0's separate inversion and octave knobs, which players found backwards (UX round 1).
  - VOICING only moves notes below the top, so it never changes the melody note TOP set (UX round 2). Checked for
    every quality and TOP.
- **SHIFT + knobs set the map:** blue MODE, green STRUM (off, 10..120 ms for the whole spread, up or down), white
  SMOOTH on/off, orange BASS on/off (the root an octave under the chord, as a 5th voice).
- **Edited key = the last key pressed.** (As built, §12: no timed freeze; a SPLIT tap pins it instead.)
  - While a key is held, the knobs edit that key.
  - The chord that is sounding, when it isn't the edited one, shows in light blue at the right.
- **Learn:** press the key, hold SHIFT, play the chord (named live, in red), let go of SHIFT.
  - The notes still held when SHIFT is let go are written; if none are held, all the notes played are. A wrong note
    can therefore be lifted before letting go.
  - The chord is stored **as played** (AS PLAYED) until TYPE is turned.
  - A learned bass that isn't the root becomes a slash bass, so G/B stays G^7/B after a TYPE edit.
  - 0 notes does nothing. More than 4 shows MAX 4 and keeps the lowest 4. 1 note maps that single note (bass or
    octave mapping).
  - Knob turns while SHIFT is held never learn.
- **LIFT** copies the edited key's chord. **DROP** pastes it onto the edited key. **SPLIT + a key** clears that key.
  - All three are only ours while the page is open; off the page they are TE's sound copy/paste.
  - **SHIFT + LIFT = undo**: one level, a snapshot of the whole map taken before the last edit gesture. Knob turns
    within 2 s, a learn, paste, clear or WRITE each count as one gesture, so a stray knob nudge doesn't lose it.
- **WRITE (SHIFT + DROP)** picks what to write into which slot:
  - blue FROM: 7THS or TRIADS from a scale, a copy of MAP n, EMPTY, or FACTORY;
  - green KEY: the tonic for a scale, the transpose for a copy, or which factory map;
  - white SCALE: major, minor, dorian, mixolydian, harmonic minor;
  - orange SLOT: the big number, defaulting to the first empty slot.

  Keys audition the preview. DROP writes; a used slot shows a red "OVERWRITE?" and needs DROP again. SPLIT cancels.
  A scale fill puts the diatonic chord on every in-scale key and leaves the others plain.
  - v0's "degrees on the white keys" layout was cut: confusing, and copy + transpose covers its use.
- **Qualities** (`c2_q`, Real Book symbols: `-` minor, triangle maj7, o dim, slashed-o half-dim):
  - triads: maj, -, sus2, sus4, o, +, 5;
  - sevenths and sixths: ^7, 7, -7, ø7, o7, -^7, 7sus4, 6, -6, 6/9, add9;
  - rootless 4-note extensions: ^9, 9, -9, 9sus4, 13, -11, 7b9, 7b13 (with BASS they get their root);
  - with root: 7#9, 7#11.
- **Recognition** names any set of up to 4 notes. All 10,752 voiced test chords get a name with the same notes, and
  close root position keeps its root and quality (`render.py` model checks).
- **Notation** is Real Book because names must fit a 22-unit key: "C^7", "F#-7", "Bø". Open question 1.

### Playing
- **Mapped key:** plays its chord (≤ 4 tones, + BASS or slash bass) instead of its note, through CHORD1's
  reference-counted relative notes. Several mapped keys at once all sound, with shared notes counted once. Plain notes
  also go through the reference count, which fixes the key-up cut (§1).
- **Octave keys** transpose everything, as today. **MIDI out** carries the chord notes: the note's flag byte sends
  MIDI (Confirmed, seq-chord.md), which makes it usable as a chord brain for an external synth.
- **HOLD (SHIFT+T2)** works everywhere, page open or not. In SHAPE/MAP it latches chords only; unmapped keys play live
  over the pad (UX round 2 blocker).
  - Keys pressed while another is still down form one gesture.
  - A new gesture replaces the held one: new notes on first, then the old ones off, so common tones are not
    retriggered.
  - v0's separate LATCH option was cut, since 6 voices can't stack two 4-note chords anyway.
- **SMOOTH** (per map): each chord takes the TOP, in its own voicing family, that moves least from the last chord,
  near the register its own TOP sets (`c2_smooth`; the check: ii-V-I in F moves ≤ 4 semitones in total per change).
  - It resets after 2 bars of silence at the current tempo (`te_tempo_spb`, `0x01AADAD8`, used by pingpong) or when
    HOLD is turned off. A phrase therefore always starts where the player set it, and a take can be repeated.
  - v0 used a 2 s reset, which fired mid-phrase at slow tempos (UX round 2).
- **STRUM** (per map): STRUM is the whole spread, so a 4-note chord at 20 ms lands within 20 ms. UX round 2 had found
  per-note gaps landing late. Up or down. The release is together.
- **Voice budget:** our code tracks all held notes, its own and plain keys.
  - Before a note-on would exceed 6, it sheds in this order: the oldest BASS voice, then the oldest plain note, then
    the oldest whole chord. TE never steals (its stealing leaves chord fragments, and a stolen note never comes back,
    see §5).
  - Releasing voices are not counted, because TE steals those first.
- **Sequencers and the arp** take keys before CHORD (as today). The badge and chip say "SEQ" instead of a chord name,
  so a mapped key giving single arp notes isn't a mystery. A chord-aware arp and sequencer is phase 4.
- **Mono voice modes:** a chord on a mono/legato sound plays its top note only. Unison plays the chord's top note
  (Hypothesis: mode byte `0x01B60EF8[slot*16] >> 12`).

### Presets
- **8 global map slots + one SHAPE per slot.** "Per sound" was rejected (§4: patch JSON size risk). "Per project"
  doesn't exist on the OP-1. A map is linked to a sound slot by that slot's MODE.
- **Factory maps** (`chord2_factory.h`). WRITE > FACTORY restores any of them; slots 5..8 start empty.
  | slot | name | keys (F3..E5) | settings |
  |---|---|---|---|
  | 1 | DIATONIC C | F3 F^7, G3 G7, A3 A-7, B3 Bø7, C4 C^7, D4 D-7, E4 E-7 and again an octave up; black keys plain | close |
  | 2 | II-V-I | F3 G3 A3 = G-7 C7 F^7 (F); B3 C4 D4 = C-7 F7 Bb^7 (Bb); E4 F4 G4 = F-7 Bb7 Eb^7 (Eb); A4 B4 C5 = D-7 G7 C^7 (C); D5 Eb5 E5 = Dø7 G7b9 C-6 (C minor), left to right | SMOOTH |
  | 3 | NEO SOUL | F3 F-9, G3 G7#9, Ab3 Ab^9, Bb3 Bb13, C4 C-9, Db4 Db^9, D4 D-7, Eb4 Eb^9, E4 Eø7, F4 F-11, G4 G7b9, Ab4 Ab6, Bb4 Bb7sus4, C5 C-11, Db5 Db^7, D5 Dø7, Eb5 Eb6 (flat roots on black keys) | SMOOTH, BASS, strum 20 ms |
  | 4 | POP | C major triads on the white keys; borrowed chords on the black ones: F#→Fm, Ab, Bb, C#→D (V/V), Eb | close |
- **Scale fill** ("fill this key's scale with diatonic 7ths"): WRITE > 7THS > KEY > SCALE > DROP, 4 inputs from the page.

## 3. Screens (mockups: `design/chord2/`, gallery `render.py OUTDIR` → `OUTDIR/index.html`)
- **Immediate mode, no SVG:** the page and the chip must draw over any TE screen, and names change every frame.
  - Text uses our stroke font plus chord glyphs (`gen_chord_font.py`: `#`, flat, triangle, o, ø, and the LIFT / DROP /
    SPLIT key icons), with a per-glyph advance so "F#-7" fits a key.
  - The draft draws 14..76 AA segments per scene (budget ~80) plus 50..500 1-px fast lines (nearly free), as counted
    by `render.py`.
  - If the keyboard art ever moves to an SVG, the SVG rules apply: one subpath per element, `check_svg.py`,
    `svg_cells.py --check`.
- **Page layout (MAP):**
  - y 4..18: the header. MAP n in blue (SHIFT+blue picks it) and the name; at the right, only what is on: strum glyph +
    ms (green), SMOOTH (white), B (orange, BASS), boxed HOLD (red, as sampler.svg).
  - y 24..47: the edited chord's name, step 3, white. A blue underline sits under the root and a green one under the
    quality, so the name shows which knob edits which part (no ROOT/TYPE readouts: UI review).
  - Right of the name: the 3-octave ruler (C ticks tall, black-key ticks purplegrey). Marks: root blue, top note white,
    others light blue. Under it, TOP ("B4", white) and VOICING (orange): the white and orange knobs.
  - y 54..61: the edited key ("C4"), with a white padlock while frozen, then a message (white confirmations, "AS
    PLAYED"). At the right, the sounding chord in light blue if it is not the edited one.
  - y 66..138: the 24 keys, white keys 22 wide and black keys 18 wide.
    - Black keys are black inside with a grey frame, and shorter.
    - Mapped keys are labelled root (a size up) over quality; unmapped keys are blank.
    - Labels are white on the edited or sounding key, grey otherwise.
    - Sounding keys are filled dark blue in their visible shape. The edited key has a 2-px white outline that follows
      its notched shape.
  - SHIFT held: the name and ruler area shows the map's four settings in knob colours (label grey, value step 1.5).
- **Colours:** blue / green / white / orange = the four knobs only. Red = HOLD, LEARN, overwrite. White = edited and
  confirmations. Dark / light blue = sounding. TE's "orange" renders coral, so it is never used for warnings (UI review).
- **Learn:** the name, ruler marks and a boxed LEARN tag in red, followed by the target key.
- **WRITE:** "WRITE" (white), the source big (e.g. "F MAJOR", "MAP 2 +2", "NEO SOUL"), the target slot as a big
  orange number at the right, the DROP / SPLIT icons top right, one line of FROM / KEY / SCALE in knob colours, and the
  previewed map on the keys.
- **Badge** (our engine screens; replaces CHORD1's box under HOLD): a frame, blue for MAP and white for SHAPE (red while
  learning), holding the map number and the last chord ("1 G7"), or "SEQ".
- **Chip** (every TE screen, frame hook):
  - A sounding chord shows top right, at step 2, on a black backing, with a blue or white frame. It stays ~1 s after
    release, in grey.
  - While silent, the slot's mode shows dim ("MAP 3"), so a slot left in MAP is visible.
  - It must clear TE's own top-right elements: check on the device.
- Scenes rendered: map_play, map_edit_top, map_edit_voicing, frozen_playing, learned_slash, map_shift, learn, learned,
  map_251, map_neo, map_pop, write_scale, write_copy, write_confirm, shape, unmapped, cleared, badge, badge_seq, chip,
  chip_idle.

## 4. Persistence
Recommendation: **our own file on YAFFS, global, A/B with CRC.** Static findings (probe_save):
| option | verdict |
|---|---|
| **a) our file** `/yaffs2/user/overdub/chords_a.bin` / `_b.bin` | **Recommended.** TE never opens it, so nothing of TE's can assert on it (Confirmed: no TE path reads it). TE writes files with `yaffs_open(path, 0x241 /*WRONLY|CREAT|TRUNC*/, 0x180)` (snapshot `.raw` `0x019AD3C4`, file copy `0x0199FD68` + write `0x0199FD96`; Verified); mkdir `FUN_01a9a886(path, 0)` (boot `0x0196F874`, snapshot `0x019AD3AC`; Confirmed). Every yaffs call takes a global semaphore (`FUN_01a98b3c`/`b78`, handle `0x01DFF368`; Verified), and TE's snapshot save opens and writes files from the UI thread (Confirmed). Outside `/user/audio` (filesync might prune unknown files there; Hypothesis). |
| b) a row in the user DB `knobs` table | Second choice. TE itself does `REPLACE INTO knobs VALUES ('%s','%s')` (`0x0196940A`) and never enumerates the table (Verified), so an extra row is ignored by stock. But TE's exec asserts on any SQL error (db2.cpp:165), the text goes through a printf format (no `%` or `'`), and its getter asserts on non-JSON (db2.cpp:707): our blob would have to be a hex JSON string, and a mistake asserts. |
| c) per sound, in the synth patch JSON | No. Unknown keys are ignored by the loader/validator (Confirmed), but patch SQL is built in 8 KB buffers (`0x01D00BC0`): a 1-2 KB blob risks an overflow assert at save, and it would be copied into every snapshot. |
| d) per project | Does not exist: the OP-1 has one tape; `tape.db` holds `tape` and `clips` only. |
- Format: magic `OVDBCHRD`, version, sequence number, length, CRC-32 (table-driven, no division), then 8 maps x 24 keys
  x 10 bytes (`struct c2_chord`: root, quality, family, top, iv[4], n, slash) + 8 names (12 chars) + per map
  (strum, direction, smooth, bass, flats) + per slot (mode, its SHAPE chord). About 2.2 KB.
- Write: from the GUI thread (our frame hook) ~2 s after the last change (UX round 1: closing the page was too late, a
  power-off with the page open lost the edits), alternating A/B (the newer valid sequence wins; no rename is known).
  Deferred while the tape records (flag to find; Hypothesis). Write cost: one or two YAFFS chunks.
- Read: once, on the first frame after boot (lazy, GUI thread). Missing, short or bad CRC/version -> factory maps.
  No assert path: open returns -1 for a missing file (Verified at TE's own existence checks, svg-route.md).
- A factory reset may delete it (Hypothesis): the factory maps come back.
- The per-slot MODE (OFF / SHAPE / MAP n for each synth slot 1..8) lives in the same file. It follows the slot, not the
  sound: loading another sound into slot 3 keeps slot 3's map.

## 5. Polyphony and voices
Static findings (probe_save), all for poly mode (voice mode 0):
- **6 voices** (`0xFF80105C[6]` free flags; VoiceCtl.cpp:178 `voice<6`). Verified.
- Note-on (`FUN_01a84604` from `FUN_01a85750`): dropped while VoiceCtl is locked; **a note already in the active
  list is ignored** (no retrigger, no second entry); else the lowest free voice (envelope finished). Confirmed.
- **Stealing:** the voice with the largest ADSR age: releasing voices are set to `0x10000000`, so the voice releasing
  longest goes first, then the oldest held. Confirmed. **A stolen note stays in the list with voice -1 and never gets
  a voice back**; its note-off only removes the entry. Confirmed.
- Mono/legato modes (1, 2) use voice 0 with last-note priority; unison (3) all 6. A chord on a mono sound plays its
  last note only: show "MONO" on the chip and play the chord's top note (Hypothesis: mode byte `0x01B60EF8[slot*16] >> 12`).
Design consequences:
- 4 notes per chord leaves 2 voices for legato changes, release tails and a melody note.
- **Voice budget (ours, all held notes):** before a note-on would exceed 6, shed the oldest BASS voice, then the oldest
  plain note, then the oldest whole chord (newest wins). TE would otherwise steal single voices and leave fragments
  (and stolen notes never come back). Release tails don't count (TE steals those first anyway).
- Plain notes go through the same per-relative-note refcount as chord notes. Today a plain key whose note is already
  in a sounding chord is ignored by TE on the way on, but its key-up would cut the chord's note.
- Velocity is applied in the voice loop to every engine (envelope gain = velocity x ADSR level; `0xFF801064[v]`,
  Confirmed); MIDI out carries it too. CHORD can therefore set per-note level (phase 4: VOICING BALANCE, softer inner
  voices, or a strum that softens later notes). Velocity >= 1 (0 would take a voice and stay silent). The keyboard
  itself has no velocity: this is shaping, not expression.

## 6. Feasibility per piece
| piece | hooks / state | proven | unknown | risks |
|---|---|---|---|---|
| MAP play, SHAPE, names, TOP/VOICING, SMOOTH, BASS, voice budget, HOLD-chords | CHORD1's note-on CALL `0x019AC290` and key-up `0x019AC3A6` (shipped); new state: maps, per-key chords | hooks shipped and working on device; voice behaviour (static) | none needed | KeyHandler CPU: a few hundred cycles per key; SMOOTH tries 11 TOPs (~2 k cycles) |
| CHORD page: knobs | repoint synth controller encoder slot `0x019119C0` (+0x2C, all four pages) to a wrapper: page open -> ours, else TE's `0x01A505BC` | upstream path Verified (KeyHandler, `FUN_019b6c0c` -> +0x2C) | delta per detent: smallest step +-1024 (Hypothesis: 1 detent) | TE's patch write skipped while open, so nothing marks the sound edited |
| CHORD page: keys | SHIFT+SPLIT (0xC), SHIFT+LIFT (0xA), SHIFT+DROP (0xB): the synth SHIFT slot, already ours (`hold_shiftkey`, vtable `0x019119B0`); plain LIFT / DROP / SPLIT while open: repoint the synth press slot (+0x14, `0x01A51058`) to a wrapper; SPLIT + note key: our press wrapper sets a SPLIT-held flag and a release-slot wrapper (+0x24, `0x01A5056C`, always returns 0, Verified) clears it, and the note-on hook clears that key instead of playing; T1..T4 close the page and go on to TE | key ids Verified; 0xC and SHIFT+0xC unused by TE on the synth screen (Verified) | none | we must pass every other key unchanged (R0..R2) |
| CHORD page: screen | wrap the frame render CALL `0x019AB8B2` (call TE, then draw; raster valid until the flush) | call chain Verified (renders the one active screen) | device behaviour, cost per frame | page draws over TE's screen: needs an opaque backing (fast lines, ~300 runs); our draw time adds to every frame while open |
| chip on TE screens | same frame hook | as above | as above | must stay clear of TE's own elements: top right, 3..22 |
| strum | wrap the sequencer thread's `CALL 0x01A431AA` (woken from the DSP core; `FUN_01a0d2c0` runs whether or not a sequencer plays, Confirmed): a per-block tick that issues queued note-ons | stock note-ons come from this thread (arp, pattern; Verified) | how often it is woken (Hypothesis: once per audio block, ~1.45 ms) | **VoiceCtl has no lock** and KeyHandler (prio 26) can pre-empt it (27). Stock has the same race (arp vs keyboard). Mitigation: while strum is on, every chord note-on/off goes through one queue drained in the sequencer thread |
| persistence | YAFFS open/write/read/close/mkdir from the GUI thread | addresses Verified at TE call sites | GUI-thread write timing; write during tape recording | a write stall during recording (shared semaphore; Hypothesis) |
| encoder pushes (optional) | key events 0x3B..0x3E (GPIO bits 8..11, Verified), unhandled on the synth screen | event path Verified | **whether this hardware has push switches** | none if absent (just unused) |
| velocity balance (later) | the `vel` byte of our note structs | voice loop applies it (Confirmed) | — | MIDI out sees our velocities |

Boot safety: every new hook is a pointer or CALL redirect checked by build_slot.py against the stock value, as
HOLD/CHORD1 do; nothing runs at boot except reading our file lazily on the first frame. No TE parser ever sees our
data (own file, no DB row, no patch key, no SVG). Saved sounds are untouched (no knob or patch writes; the page
swallows knob turns instead of editing the sound). The feature mask grows a bit (FEAT_CHORD2) so apply.py can leave
it out; off = CHORD1 behaviour.

## 7. Hard rules
- No runtime division: the draft model and screens compile for Blackfin (`op1-bfin`, GCC 4.3, -O2) with no libgcc
  calls (only the three line primitives are undefined); modulo 12 is a compare loop.
- Images never write the bootloader: no te-boot change anywhere in this plan.
- Every probe or test image registers every engine the user's slots and saved sounds use (LOG.md test image 61 rule).
- SVG: none needed (immediate mode, so it works over TE's screens). If a later version moves the keyboard art to an
  SVG: one subpath per element, `check_svg.py` and `svg_cells.py --check` at build.

## 8. Device probes first (one image, CHORDPROBE)
1. **Frame hook**: a small test chip drawn on every screen (synth, drum, tape, mixer, FX, LFO); report frame cost
   (CYCLES around our draw) and whether anything flickers.
2. **Encoder slot swap**: a flag (SHIFT+SPLIT) that swallows turns on all four synth pages and counts them with the
   raw delta (confirms +-1024 per detent).
3. **Press slot wrapper**: SHIFT+SPLIT, LIFT, DROP, SPLIT counted while the flag is on; TE's own LIFT/DROP still work
   when it is off.
4. **Encoder pushes**: count key events 0x3B..0x3E.
5. **Sequencer tick**: count wake-ups per second with no sequencer playing and with arp playing; a 4-note test strum
   (20 ms) from that thread.
6. **File**: mkdir `/yaffs2/user/overdub`, write 2 KB A/B, read back, CRC; survives a power cycle; time per write.
7. **Velocity**: a test chord at velocity 127 vs 48 on a TE engine: audibly softer.

## 9. Build plan (MVP first; rough effort in focused days, host tests included)
| phase | what | needs | effort |
|---|---|---|---|
| 0 | **CHORDPROBE** image: the seven probes of §8, each visible on a test screen. Built like the current release + probe code; registers every engine. | — | 1.5 |
| 1 | **MVP**. The engine: `chord2.c` replaces `chord_keydown`'s shape path with maps, keeping CHORD1's reference counts and stale purge. Play: per-slot MODE, MAP 1..8 + SHAPE, plain notes in the reference count, the voice budget, HOLD chords-only + replace. Page: SHIFT+SPLIT; the four chord knobs (ROOT, TYPE, TOP, VOICING) and SHIFT+blue MODE via the +0x2C swap; learn on SHIFT release (held notes, AS PLAYED, slash); SPLIT+key clear; SHIFT+LIFT undo. Factory maps 1..4 in the slot block. A/B file save/load. Screens: page, badge, chip (only if probe 1 passes; else page and chip on our engine screens only, phase 1b). Host tests extend `hold_test.c`: maps, budget, learn, undo, file round trip and corruption, every mask combination. `FEAT_CHORD2` in `_feature_mask`; apply.py lists it. | probes 1-3, 6 | 6-8 |
| 2 | **Map tools**: SMOOTH (tempo reset), BASS, LIFT/DROP copy/paste, the WRITE page (scale fill, copy + transpose, factory restore, empty, overwrite confirm). | phase 1 | 3 |
| 3 | **STRUM**: queue drained by the sequencer-thread tick; while on, every chord note-on/off goes through it (one thread, no VoiceCtl race). | probe 5 | 2 |
| 4 | Later, if wanted: a chord-aware arp and sequencers (expand chords at the players' note calls, or a `chord` sequencer type, seq-chord.md option B); velocity balance; MIDI-in chords; encoder pushes (audition / reset), if probe 4 finds them. | — | 3-6 each |

Risks to watch:
- **Boot safety:** every hook is a checked pointer or CALL redirect, and nothing new runs before the first frame. The
  file is read lazily and validated (magic, version, length, CRC): missing or bad means factory maps, never an assert.
- **Saved sounds:** untouched. No patch or knob writes; the page swallows knob turns.
- **CPU:** the KeyHandler work is small. The page draw while open is ~30-76 AA segments (comparable to our engine
  screens). The chip is ~20 AA segments on every TE screen while visible: measure in probe 1. If it is too costly, draw
  the chip with fast lines only.
- **UI thread:** file writes are deferred and debounced, and never run while recording.
- **Feature off:** `apply.py --without chord2` gives CHORD1 exactly.

## 10. Review log
Four rounds, each by a fresh subagent; the fixes are in the design above and in the mockups (v0 → v1 → v2).
- **UX round 1** (an OP-1 owner of many years, keys/jazz/neo-soul; played the scenarios "ii-V-I in F one-handed",
  "neo-soul pads over a drum loop", "edit one chord's voicing mid-song", "C4 → Cmaj7, A3 → Am7 then a melody", "next day
  after power-off", "SHAPE vs MAP with HOLD").
  - Blockers:
    - no way to choose OFF / SHAPE / MAP, and LATCH had no control → SHIFT+blue = MODE (OFF, SHAPE, MAP 1..8), LATCH
      cut (HOLD replaces);
    - chords could be on with nothing on screen → boot to OFF with the mode remembered.
  - Majors:
    - melody notes let TE steal chord voices → our budget owns all notes;
    - saving only on page close → save 2 s after a change;
    - SPLIT clear one slip away from SHIFT+SPLIT → clear is SPLIT + key, plus undo;
    - the edit target drifts while playing → 2 s freeze and held key wins;
    - SMOOTH fought VOICE → per map, deterministic;
    - VOICE (inversions) didn't match how players think → the TOP ladder on white, the VOICING family on orange;
    - FILL overwrote and its white-key layout contradicted itself → WRITE to the first empty slot, confirm on a used
      one, white layout cut;
    - sequencers bypass chords → later phase.
  - Added: BASS option, new/empty and factory restore (WRITE), undo, 6/9, 9sus4, 7#11, 7b13, learned chords kept as
    played, MAX 4 message, single-note mapping, minor ii-V-i in key order.
- **UX round 2** (a live hardware performer: OP-1, Digitakt, Prophet; house, lo-fi, ambient; played "ambient pad set
  then a lead in another slot", "house progression with a slash chord from empty", "widen and raise one chord
  mid-song", "make mistakes and recover", "with the arp", "a dark stage").
  - Blockers:
    - a global mode chorded the lead slot → mode per synth slot;
    - HOLD with melody notes was undefined → HOLD latches chords only, plain keys play live.
  - Majors:
    - learn obeyed the freeze → SHIFT ends the freeze;
    - VOICING moved the top note → drops keep the top (checked);
    - the SMOOTH 2 s reset fired mid-phrase → reset after 2 bars of tempo or HOLD off, and TOP always shows the anchor;
    - strum landed late → STRUM is the whole spread, NEO 20 ms;
    - the arp bypass was silent → "SEQ" on the badge and chip;
    - the mode was invisible on TE screens when silent → the dim mode chip;
    - readability → root labels a size up, black-key interiors black, a bigger padlock;
    - no map copy or transpose → WRITE FROM MAP n + transpose.
  - Minors adopted: blank placeholders; common tones not retriggered on replace; release tails not budgeted; slash
    bass kept through TYPE edits; learn takes the notes still held; undo is per gesture and covers WRITE; BASS shed
    first.
  - Cut: SHAPE as separate code (now a uniform map; the CHORD1 gesture kept); the 2-octave BASS.
- **UI round** (a TE-style interface designer, on the v1 PNGs and the draw code).
  - The ROOT/TYPE readouts duplicated the name → removed. Blue and green underlines under the name now show which
    knob edits which part. TOP and VOICING moved under the ruler, and the readout row is kept only for the SHIFT
    layer. The keyboard moved up 14 units.
  - One colour rule:
    - edited = white, sounding = (light/dark) blue;
    - orange = the 4th knob only, red = HOLD / LEARN / danger;
    - confirmations are white;
    - badge and chip frames are blue (MAP) or white (SHAPE).
  - Bright blue black-key fills killed their labels → dark blue for both colours of key.
  - The sounding chord is named when it isn't the edited one.
  - Ruler: root blue, top white, marks touching the ticks, no stray octave label, purplegrey black ticks.
  - Keyboard: a notched outline for the edited key (fast lines); black keys 18 wide, black inside.
  - Sentences became words or glyphs (LEARN tag, MAX 4, LIFT / DROP / SPLIT icons in the font).
  - The status row is glyphs, and the WRITE page was trimmed: write_confirm went from 92 to 76 AA segments.
- Not adopted: number keys 1..8 as map select (they stay TE's sound slots; SHIFT+blue is enough); encoder pushes
  (not proven to exist).

## 11. Open questions for the user
1. **Chord notation on screen:** Real Book symbols (C^7 as a triangle, A-7, Bø) are what fits the 24 key labels.
   Spelled-out names ("CMAJ7", "AMIN7") would only fit in the big title. Is the jazz notation OK everywhere?
2. **Priority after the MVP:** live playing (phases 2-3: SMOOTH, WRITE, STRUM) or beat-making (phase 4: chords through
   the arp and sequencers)? This decides the order after phase 1.
3. **Power-on:** once the TE-screen chip is proven, should each slot come back in its last mode (MAP 3 on the Rhodes
   slot), or always start OFF?

## 12. MVP as built (2026-10-17)
Code: `src/slot/chord2.h` (model, state), `chord2_impl.h` (engine, page keys and knobs, learn, undo, the file),
`ui_chord2.h` (page, badge, chip), `ui.c` (`c2_frame`, the frame hook's C side), `hold_impl.h` / `hold.c` (dispatch),
`entry.inc` `C2_HOOKS` (asm entries), `build_slot.py` (patches), `chord_font.h` (`gen_chord_font.py`). Feature bit
`chord2` = 4 in `_feature_mask` (now 7); it only acts with `chord` (2). `apply.py --without chord2`: CHORD1 exactly, every
new hook calls TE's function with the same arguments (simulator-checked).

### Controls
| where | control | does |
|---|---|---|
| synth screen | SHIFT + SPLIT | open / close the CHORD page (T1..T4 also close it and go to their page) |
| page | press a key | plays as normal and becomes the edited key, chord or plain note, at once (no freeze; NIGHT2), unless pinned |
| page | SPLIT tapped alone (< ~0.5 s) | pin / unpin the edited key (blue padlock, blue outline; "UNPINNED: LAST KEY EDITS" with an open padlock): play the song, keep editing one chord. Closing the page unpins; it opens unpinned |
| page | blue | ROOT (semitones from the key; a learned chord transposes) |
| page | green | TYPE: the scan (below); left past the first = the key plays its note |
| page | white | TOP: each detent puts the next chord tone on top |
| page | orange | VOICING: close, drop 2, drop 3, spread (the top note kept) |
| page | SHIFT + blue | MODE of this synth slot: OFF, SHAPE, MAP 1..8 |
| page | SHIFT + orange | BASS on / off for the map (the root an octave under, a 5th voice; shed first) |
| page | SHIFT + green / white | STRUM / SMOOTH: "NOT IN THIS VERSION" (phase 2/3) |
| page | hold SHIFT, play, let go of SHIFT | learn into the edited key (the last key tapped; pinned: the pinned one; the tapped key is only the target, not a note of the chord) (held notes; none held = all played; 5 notes = 4 + its bass; more = the bass and the top 4, MAX 5). A SHIFT combo during a learn (LIFT, DROP, SPLIT, T2) cancels it |
| page | SPLIT + key | clear that key (the press plays nothing; afterwards the key plays its own note) |
| page | LIFT / DROP | copy the edited key's chord / paste it onto the last key tapped (pinned: copy the pinned chord, tap keys and DROP to fan it out; the pin stays) |
| page | SHIFT + LIFT | undo the last gesture (again = redo); turns of one knob within ~2 s are one gesture |
| page | SHIFT + DROP, then DROP | restore this map to factory (maps 5..8: clear it). SHIFT+DROP asks "MAP n TO FACTORY: ] AGAIN" (red, shown while SHIFT is held); only a plain DROP within ~2.4 s confirms; any other key, knob or SHIFT combo cancels; SHIFT+LIFT undoes. The full WRITE page is phase 2. Hint: "\ PIN  SHIFT+] FACTORY" in the SHIFT layer's header |
| anywhere (synth) | SHIFT + notes (page closed) | slot OFF or SHAPE: CHORD1's gesture, learn a SHAPE (one note = slot OFF); MAP: just plays (never drops MAP mid-song) |
| anywhere (synth) | SHIFT + T2 | HOLD: latches chords only (everything when the slot is OFF); a new chord replaces the held one |
- Power-on: each slot comes back in its last mode (user decision 3). The image's default: every slot OFF.
- **Notes view** (NIGHT2, replaces the 3-octave ruler, which was too small to read): while a chord knob (ROOT, TYPE,
  TOP, VOICING) turns and ~1 s after the last turn, after a learn (~1.5 s), during a learn on the page (red) and always
  in SHAPE, the 24-key keyboard shows the edited chord's notes instead of the map's names: each note's key filled
  (root blue, top note white, others light blue) and named in black; notes off the keyboard as an arrow at that edge,
  with the count when more than one. Then it switches back to the map view (no fade: the panel has no alpha). The freed
  top: the chord name at x4 (shrinks to fit), TOP ("TOP B4") and VOICING right-aligned beside it.
- Names: jazz symbols on the keys (C△7, A-7, Bø), spelled out in the big title where they fit ("C MAJ7", "A MIN7",
  "G7"; else the symbol), user decision 1.

### TYPE scan (Model:Cycles comparison, 2026-10-17)
The user asked whether one knob that scans an ordered chord list (the Cycles CHORD machine's SHAPE) beats the scope's
ROOT / TYPE / TOP / VOICING. Verdict: **hybrid (option b)**. The green knob is now a Cycles-style scan, and the other
three knobs stay for precision.
- The list (`chord2.h` enum order, 28 qualities, after UX round 1): NOTE | MAJ, MIN, MAJ7, 7, MIN7, ø7, DIM, DIM7 |
  6, MIN6, MIN^7 | SUS4, SUS2, 5, 7SUS4, 9SUS4 | ADD9, 6/9, MAJ9, 9, MIN9, MIN11, 13 | 7#11, 7b9, 7#9, 7b13, AUG. The
  everyday triads and sevenths are within 8 detents of NOTE (MAJ <-> MIN is one detent); then sixths, the open (no
  third) group, the colour chords, the altered ones. Leaving MAJ for NOTE takes two detents within ~1 s (a wall), so a
  scan can't clear a key by accident: the second detent counts only after resting at MAJ for ~0.3 s ("AGAIN = NOTE"),
  so a fast left spin stops at MAJ.
- Rootless colour qualities (MAJ9, 9, MIN9, MIN11, 13, 7b9, 7b13, 9SUS4) add their root right under them when the map's
  BASS is off (shed first, like BASS), so every scan step sounds its root, as on the Cycles.
- Audition: on a held (or HOLD-latched) key the chord changes as the knob turns. Only the notes that change retrigger,
  so a scan is a smooth morph, not a stream of re-attacks. Nothing sounds from a knob alone (no stray notes on stage).
- The title shows the full name; while green turns, a 29-tick strip replaces the green underline (taller ticks mark the
  family starts, the current one green).
- Why not the Cycles knob alone: the Cycles transposes one shape to the played note, which is what SHAPE mode does
  (SHAPE uses the same scan). A map stores a different chord per key, and its value is the voice-led choices:
  ii-V-I tops, drop-2s, slash basses. A single list would multiply 28 qualities by 4 voicings by ~8 tops. The scan
  covers the fast, playful part; TOP and VOICING stay one knob each.

### Hooks (all installed with --chord; checked against stock by build_slot.py / ldr_patch.py)
| hook | site | stock | evidence | if the assumption is wrong on the device |
|---|---|---|---|---|
| frame | CALL at `0x019AB8B2` in the GUI loop `FUN_019ab880` | `FUN_019b2674` (render, then flush `0x01AA185C`, flip `0x01A2FB18`) | **Verified**: loop disassembly; R0 = the back buffer, reloaded after the call | page and chip don't show; the page falls back to our engine screens (`FB` on the diag line); no file I/O (factory maps, no save) |
| press | synth controller vtable `0x01911994` +0x14 (`0x019119A8`) | `0x01A51058` | **Verified**: `FUN_019b6c74` calls +0x14 for presses without SHIFT, else the global `FUN_019ac4b4`; keys 2..0xB are a jump table (6..9 = T1..T4, 0xA LIFT, 0xB DROP), 0xC (SPLIT) and note keys (>= 0x3A) return 0 | SPLIT / LIFT / DROP counters stay 0 on the diag line; LIFT / DROP stay TE's |
| release | +0x24 (`0x019119B8`) | `0x01A5056C` | **Verified**: called for every release before the global `FUN_019acd60` (which clears SHIFT at `0x019ACF94`); returns 0 except 0x24 / 0x25 | `UP` stays 0; learn then ends at the first plain note (fallback) |
| encoder | +0x2C (`0x019119C0`) | `0x01A505BC` | **Verified**: `FUN_019b6c0c(knob, int16 *delta)` adds 4 with SHIFT (`FUN_019d7c6c` = `B[0x01DD7F78]`) and calls +0x2C; delta = (count & ~1) << 9 (`0x019C2E7C`) | `K` stays 0; knobs keep editing the sound under the page (seen at once) |
| SHIFT slot | +0x1C (`0x019119B0`) | `0x01A50904` | CHORD1's, shipped. **Verified (NIGHT1's learn bug):** `FUN_019b6c74` sends *every* press with SHIFT down here first, note keys included; TE's then returns 0 and the global `FUN_019ac9b8` plays the note (`FUN_019c316c` is-note, `FUN_019c3322` code -> index, `FUN_019ac0b0`). Note key codes (table `0x018E0A80`): 0x0D..0x0F, 0x16..0x20, 0x27..0x2A, 0x30..0x35 | — |
| note on / off | CALLs `0x019AC290` / `0x019AC3A6` | | CHORD1's, shipped | — |
- Every hook runs in the KeyHandler thread except the frame hook (GUI thread). The GUI writes the maps only when it
  loads the file (once; `loading` set: the keys play plain notes meanwhile).
- Current controller: `[0x01DD8BC8]` (`FUN_019e9738` stores it, `FUN_019e97fc` reads it; Verified); its vtable at +4.
  The frame hook closes the page off the synth controller, but only once that check has succeeded at least once (`CT`
  on the diag line), so a wrong guess cannot stop the page from opening.

### The file
- `/yaffs2/user/overdub/chorda.bin` and `chordb.bin` (our own directory; nothing of TE's is opened for writing).
  Written alternately, never over the newest good one. yaffs calls as TE's file copy (`0x0199FD40`): open
  `0x01A997F4(path, 0 / 0x241, 0x100 / 0x180)`, read `0x01A99C90`, write `0x01A99F84`, close `0x01A99908`, mkdir
  `0x01A9A886(path, 0)` (boot `0x0196F874`). All return -1 on errors; none asserts.
- 2560 bytes: "OVDBCHRD", version 1, header size 24, sequence, payload size 2536, CRC-32; 8 maps x 24 x 12 bytes,
  8 settings x 16, 8 shapes x 12, 8 slot modes. A file that is short, long, of another version, with the wrong magic
  or a bad CRC is not used (the other one, else the factory maps). Every value read is clamped (a random payload
  with a valid CRC is tested). The newest by sequence number (wrapping) wins.
- Load: frame 150 (~5 s after boot), from the frame hook; if neither file can even be opened it retries 3 times,
  256 frames apart (the disk may not be up), and only then uses the factory maps. No save before a load finished.
  Every save re-reads both headers first and writes over the older one with the next sequence number. Save: 64 frames (~2 s) after the last edit, not while
  learning, from the frame hook (between TE's render and the flush: TE holds no yaffs lock there). A failed save
  retries after 256 frames, at most 8 times, then waits for the next edit.
- Not handled: a save during tape recording (no flag known; one 2.5 KB write).

### Voice budget, HOLD
- Our tracking: per key the relative notes it sounds, reference counted (`rel_ref`), plain notes included.
- Chord notes outside MIDI 12..115 are left out, so an octave key (TE transposes sounding voices) can't push one past
  0..127, where its note-off could not be sent (code review #2).
- Before a note-on would make more than 6 distinct notes: for a chord the oldest BASS note, then the oldest plain
  note; for a plain (melody) note the oldest plain note, then the BASS; then the oldest whole chord is let go (whole: never half a chord). A let-go key's key-up does nothing. Release tails are not
  counted. Common tones of a new chord are referenced before the old ones are released: never retriggered, never
  stolen. Fuzz-tested: 20,000 random events, never above 6, every tracked chord whole, nothing left at the end.
- Re-voicing a sounding key (knob audition) goes through the same budget.

### Diagnostics (page, SHIFT held): what the morning test reads
- Line 1: `HOOK F:<frames> K:<knob turns> D:<last raw delta> SP:<SPLIT> LI:<LIFT> DR:<DROP> UP:<SHIFT releases>
  DN:<SHIFT presses> CT|CT0`.
- Line 2: `FILE <-|NONE|A|B|BAD> L:<load tries> <seq> SAVE <count> <-|OK|OPEN|WRITE|CLOSE|TORN> MK:<mkdir> VOX <voices>
  H:<latched keys> SHED <n> PG:<purges> [FB]` (`FB` = the frame hook does not run, page drawn by our engine screen).
- Line 3 (NIGHT2): `LEARN S:<starts> OK:<written> X:<cancelled> B:<SHIFT+notes that did not learn> END:<reason>
  CB:<last combo key, hex> [USED]`. END: `U` SHIFT let go (release slot), `N` a plain note ended it (fallback), `P` a
  key press without SHIFT ended it (fallback), `C` cancelled by a combo, `E` nothing to write, `B` blocked (this SHIFT
  press was already used for a combo), `M` blocked (MAP with the page closed). CB: the key code that made the SHIFT
  press a combo (0C SPLIT, 07 T2, 0A LIFT, 0B DROP, 24/25 octave, 10x a SHIFT knob, 2xx MAP off the page); USED = the
  SHIFT press held now no longer learns.

### Cut from the MVP (and why)
- SMOOTH, STRUM, WRITE (scale fill, copy + transpose, factory restore): phase 2/3; user decision 2 puts these live
  tools next.
- "SEQ" on the badge and chip, MONO top-note: need flags not yet verified (sequencer enable, voice mode byte).
- Map names are fixed (factory names; empty maps show none).

### Factory maps as built
- 1 DIATONIC C with TOPs that keep each octave's chords in one register (tops A4..B4 low, F5..G5 high), 2 II-V-I with TOPs set so each group voice-leads (6 semitones of movement over its two
  changes, found by search: e.g. G-7 TOP +2, C7 0, F^7 +2), 3 NEO SOUL on the lower octave only (F3..E4; the upper
  octave plays plain notes for a melody over a held pad), BASS on, 4 POP. Maps 5..8 empty.

### Review rounds (MVP)
- **Code review** (fresh subagent: correctness, boot safety, threads, memory). Nothing can assert or write outside our
  state and files. Fixed:
  - a load that failed (disk not ready) made the first save overwrite the newest file: retries, no save before a
    real load, and every save re-reads both headers;
  - an octave change could push a sounding chord note past 0..127, so its note-off was never sent: notes kept
    within 12..115;
  - re-voicing could send a note-off twice: order fixed, and the fuzz checks it;
  - the save's torn-edit check: `gen` bumped at an edit's start too (volatile), the save waits 2 s after it;
  - the slash bass was clamped at +-24 on load: now +-60;
  - stale `chord_down` after a lost key-up blocked HOLD replace;
  - a learn stuck when SHIFT is let go on another screen: committed on the next press, never blocks the save;
  - a false `FB`;
  - edits racing the load.
- **UX round 1** (an OP-1 owner who knows the Model:Cycles, jazz / neo-soul / house; the seven scenarios). The hybrid
  scan is right; SHAPE uses the same scan; audition on a held key only is right. Adopted:
  - the scan reordered (MAJ first, MAJ <-> MIN one detent), with a wall before NOTE;
  - NEO SOUL on the lower octave (melody room);
  - II-V-I voice-led by its factory TOPs;
  - a SHIFT used for a combo (SHIFT+T2, SPLIT, a knob) no longer learns;
  - off the page in MAP, SHIFT + notes no longer switches the slot to SHAPE / OFF;
  - SHIFT and DROP take the last key tapped even while frozen;
  - SPLIT tapped alone pins the edited key;
  - > 4 learned notes keep the bass and the top 3;
  - a melody note sheds plain notes before the pad's BASS;
  - REDONE;
  - knobs ignored during a learn;
  - hints on the NOTE and OFF screens.
  Not adopted: SHIFT+white as a map transpose (WRITE's copy + transpose is the plan, phase 2); slash basses on the key
  labels (no room in 5 glyphs).
- **UI review** (TE-style visual designer, on the C-path renders). Adopted:
  - turned underlines keep their knob colour (wider instead of white);
  - TOP / VOICING bigger when turned;
  - the scan strip replaces the green underline (grey ticks, family starts taller);
  - labels shrink to fit black keys;
  - the diagnostic lines in a 96-byte buffer, grey, shrink to fit;
  - long messages shrink so they stay clear of the ruler;
  - SHAPE header blue, its chord tones as ruler-style marks instead of the "sounding" fill;
  - red outside a learn is always boxed; BASS OFF grey; no underline on the B;
  - fast text for static hints, HOLD, the sounding chord and the idle chip (0 AA);
  - a narrow white-key neck outlined once;
  - ruler colours swapped and marks thinner;
  - LEARN tag aligned;
  - the header baseline;
  - AS PLAYED in the VOICING slot.

- **UX round 2** (a live performer: OP-1, Digitakt, Prophet; house, lo-fi, neo-soul; knows the Cycles). No blockers.
  Adopted:
  - a plain note over a sounding chord no longer takes the edit (a melody over a pad);
  - a SHIFT combo during a learn cancels it;
  - the wall needs a rest (a spin can't fall through), with a message;
  - rootless qualities keep their root without BASS;
  - a learned 5-note voicing keeps all 5 (the lowest as its bass);
  - a learn over a latched pad sheds the pad before its own notes;
  - SHIFT alone no longer moves the edited key (only the first learn note does);
  - freeze ~4 s;
  - undo groups per knob;
  - the chip framed red while HOLD is on;
  - the pin ends when the page closes; DROP pastes where you tapped;
  - "TURN GREEN FIRST" for VOICING on an unrecognised learned chord;
  - DIATONIC C's register;
  - diagnostics: load tries `L`, latched keys `H`, stale purges `PG`, SHIFT presses `DN`.
  Not adopted: SPLIT + key as select-and-pin (the user's scope has SPLIT + key = clear; a SPLIT tap pins instead);
  group gaps on the II-V-I keys. To check on the device: TE stealing a kept common tone during long release tails
  (morning test step).
- Learned chords ignore the map's BASS (their own bass, if any, is the 5th note); a melody double-stop over a 5-voice pad
  sheds the first melody note.

- **Device test NIGHT1 (LOG.md test 84).** Everything worked (boot, frame hook, knobs, keys, file, chip, voices, HOLD,
  octave edge, TYPE scan, clear / undo) except learn, which never started ("the notes just played normally").
  - **Cause:** with SHIFT down, `FUN_019b6c74` calls the SHIFT slot (`hold_shiftkey`) for every press before TE plays
    it, note keys included. The UX round 1 rule "a SHIFT used for a combo no longer learns" marked every SHIFT + key
    there as a combo, so the first chord note set `shift_used` and `c2_keydown` then played it plain. The host test
    called the note hook directly and missed that route. CHORD1 has no such rule, so its learn works.
  - **Fixes:** note key codes (TE's own table) are not combos; the host tests now deliver every key the way
    `FUN_019b6c74` does (press slot / SHIFT slot first, then the note hook; release slot first) and reproduce the
    bug without the fix; the LEARN diagnostic line.
  - LIFT / DROP off the page: our press slot passes them to TE's own press function with the same arguments, as stock
    (`0x01A51058`: LIFT -> `FUN_01a50aa8`, copy the slot's sound; DROP -> `FUN_01a50b40`, paste). Stock TE behaviour:
    a sound copy / paste, so LIFT then DROP on the same slot changes nothing audible.
  - The ~4 s edit freeze is gone (user): the edited key always follows the last key pressed, unless pinned. The
    UX round 2 rule "a plain note over a sounding chord doesn't take the edit" went with it; the pin covers that case.
  - New (user): restore a map to factory (SHIFT+DROP, then DROP); the notes view on the main keyboard instead of the
    ruler.
  - A bfin GCC 4.3 `-O2` miscompile found on the way: a per-key loop around the inlined byte copy from the const
    factory table left every key after the first zero in the simulator (`-O0` and the host were right). The restore
    copies the whole map in one flat loop instead, and the simulator test checks it.
  - Root cause (a compiler bug, not UB in our code; `-fno-strict-aliasing` does not change it): the loop was
    `for (k) { if (m < 4) copy key; else clear key; }`. GCC hoisted `m < 4` out of the loop into the CC register.
    The outer loop did not become a hardware loop (block reordering gave it two latches), so bfin's machine reorg
    rewrote its `loop_end` insn as `P4 += -1; cc =P4==0; if !cc jump`. That overwrites CC, which `loop_end` never
    declares. From the second key on, `if !cc` tested the counter, so the clear branch ran. `-O1`, `-O3` (which
    unswitches the loop), `-fno-move-loop-invariants`, `-fno-gcse` and `-fno-reorder-blocks` each avoid it. Whether
    it hits depends on context: the same loop inlined into `c2_press` became a hardware loop and was right; it only
    broke where `c2_restore` stayed out of line. Repro, an assembly scan for the signature, and a scan of every
    device object: `src/slot/test/loopcc_check.py` (`loopcc.py`, `loopcc_main.c`). The scan found one more instance,
    modal.c's `strike` dry run (`plain ? w : lp` in the loop), which is now a mask select. Non-pluck bursts had
    been normalised on unfiltered noise: ~1 dB under their target RMS when bright, ~25 dB when dark. modal is no
    longer in engines.json, so no current image (NIGHT2 included) has it; the MODAL test images (LOG.md image 23
    on) probably did.
- **UX round 3** (fresh subagent, live performer, on the new rules and renders). Adopted:
  - pinned, DROP pastes onto the key tapped (LIFT the pinned chord, tap, DROP: fan it out);
  - the factory question names the map ("MAP 3 TO FACTORY", "CLEAR MAP 6"), and only a plain DROP confirms (SHIFT+DROP
    twice only asks again);
  - only a short SPLIT tap pins (holding SPLIT to clear, then letting go unused, no longer toggles the pin);
  - "\ PIN" next to the factory hint; "UNPINNED: LAST KEY EDITS".
  Not adopted: a dedicated restore undo slot (one-level undo, like every other gesture); naming the key and chord in
  LEARNED (the big name already shows it).

### Checks
- `src/slot/test/chord2_check.py`: the engine test (`chord2_test.c`, 203 checks; keys delivered as `FUN_019b6c74` does: model, factory maps, play, budget
  fuzz, HOLD, page, learn, file: missing / short / long / old version / magic / CRC / random payload / sequence wrap /
  write failures, every feature mask) under the Blackfin simulator; the asm entries' ABI and pass-through
  (`chord2_main.c`); hold.o calls no libgcc / libc.
- `src/slot/test/hold_check.py`: CHORD1 unchanged with chord2 off.
- `src/slot/test/chord2_screens.py OUTDIR`: the gallery from the device draw code; nothing outside the safe area,
  <= 120 AA segments per scene (max 65), the frame hook's page rules, the notes view's timing and colours.

## 13. Phase 2 as built (2026-10-17)
Code: as §12, plus `c2_seq_tick` (hold_impl.h) and `_c2_seq_hook` (entry.inc C2_HOOKS). One new hook, checked against
stock by build_slot.py: the sequencer thread's step CALL at `0x01A431AA` (below). File version 2 (below).

### Controls (changes from §12)
| where | control | does |
|---|---|---|
| page | SHIFT + green | STRUM, the whole spread: down 300 ms .. OFF .. up 300 ms (detents 5 10 15 20 25 30 35 40 50 60 70 80 90 100 120 150 200 250 300 ms; a wall at OFF). Header: green `{30` (low to high) / `}30` (high to low); grey while no sequencer tick has been seen (then it plays at once) |
| page | SHIFT + white | SMOOTH on (right) / off (left). Header: `SM`, white while leading on, grey while it starts over |
| page | orange | VOICING, bipolar (below). A 9-tick strip under its name: the centre tall white, the positions this chord has grey, the current one orange |
| page | green | TYPE: the MVP list right of NOTE as before (MAJ one detent right), the WIDE group LEFT of NOTE (below); a wall on each side of NOTE |
| page | SHIFT + DROP | **WRITE** (below). SHIFT+DROP, DROP is still NIGHT2's factory restore (maps 5..8: clear) |
| page | playing | the played-notes view (below) |

### SMOOTH (per map)
- Each new chord's TOP is chosen to move least from the last chord's upper notes, in the chord's own VOICING
  (`c2_smooth`, chord2.h). Cost: 4 x the movement (voice by voice when both have as many notes, else nearest note
  both ways) + the drift of the note sum from the chord's own TOP + the TOP distance. Candidates: own TOP +- n steps (an
  octave), never more than an octave of mean drift from its own register, all notes inside +-35. Deterministic.
- BASS and a rootless chord's added root are not voice-led (they stay under the chord).
- Starts over (the chord's own TOP plays): after 2 bars of silence (no chord sounding) at TE's tempo
  (`0x01AADAD8`, samples per beat Q14, pingpong's valid range, else 120 BPM: 8 beats x spb x 17.7 c2_time units per
  sample), when HOLD is turned off, on another map, after a WRITE, and when SMOOTH is turned on.
- **Anchor** (UX round 2): white turned while SMOOTH is on starts from the TOP SMOOTH just played on that key and
  anchors the chord (a white padlock by TOP; the chord byte that was padding, `flags`). SMOOTH then plays an anchored
  chord's own TOP exactly and leads on from it: the way to pin one melody note in a voice-led line. A TOP turned
  with SMOOTH off clears the anchor. Until anchored, "SM E4" in grey before TOP shows the TOP SMOOTH played.
- A knob edit auditions on the held key, and SMOOTH leads on from what was auditioned.
- SHIFT+white right while SMOOTH is on starts the chain over ("SMOOTH STARTS OVER"); no undo step.
- Check: ii-V-I in F (G-7 C7 F^7 in close root position) moves <= 4 semitones in total per change (3 and 3); an
  8-chord ii-V-I-vi chain moves a third of root position or less; fuzz: never more than the own TOP's movement.
- Time: `c2_time()` = CYCLES2:CYCLES >> 9 (1.28 us at 400 MHz, Hypothesis on the clock; wraps every ~1.5 h).

### STRUM (per map) on the sequencer thread
- **The thread:** `FUN_01a4318c` loops: pend on the semaphore at `[0x01DF50B8]` (R1 = 0: wait), then CALL
  `FUN_01a0d2c0` (the sequencer step: arp / pattern ticks, the internal clock) at `0x01A431AA`, then jump back to the
  pend (`0x01A431AE` JUMP.S `0x01A43198`, which reloads R0 / R1). **Verified** in the disassembly. `FUN_01a0d2c0`
  reads no argument register (its first instruction after the prologue is a CALL; Verified). Our `_c2_seq_hook`
  replaces that CALL: TE's step first with R0-R2 untouched, then `c2_seq_tick` through a C entry (all registers TE
  relies on preserved: chord2_main.c's ABI harness). Mask without chord2: TE's step only. How often it wakes is a
  **Hypothesis** (once per audio block, ~1.45 ms); the diagnostics show the measured rate and the longest gap.
- **Thread rule:** TE's voice control (`FUN_01a85750` / `FUN_01a85840`) has no lock and the KeyHandler (priority 26)
  pre-empts the sequencer thread (27). So while the slot's map strums, **no note of ours is sent from the KeyHandler**:
  - The KeyHandler only keeps the wanted state: `rel_ref` (as before), plus per note a due time and an onset count
    (`c2_out`, bumped on every new onset, so a re-press retriggers).
  - The tick sends the difference: offs first (a wanted note that stopped, or one whose onset count moved), then the
    untracked key-ups the KeyHandler left it (`raw_off`), then the ons that are due.
  - Nothing is queued, so nothing can arrive out of order. A note released before its strum turn never sounds. Every
    tick converges on `rel_ref`. A tick pre-empted half way finishes on its next run (at worst a ~1.5 ms blip).
  - The KeyHandler always sees and leaves whole key events, since it pre-empts the tick and runs to completion.
- **Ownership** (`c2.owner`): 0 = the KeyHandler sends (the MVP path, every map without STRUM); 1 = the tick sends;
  2 = the tick sends until nothing is pending, then sets 0 itself.
  - Handover (0 -> 1, KeyHandler, at a key event or knob turn) only once the GUI has seen the tick run for 4 frames in
    a row. It copies what sounds into the tick's `on[]` first.
  - **Fail-safe:** if the tick then stops for 3 GUI frames, the next KeyHandler event takes the notes back (owner 0)
    and sends the difference itself (`TO:` on the diagnostic line).
  - No tick ever seen (hook not running): STRUM is ignored and the notes play at once (header strum grey, "STRUM: NO
    SEQ TICK, PLAYS AT ONCE").
  - A clock that does not move: a wanted note is sent after 250 ticks whatever its due time (`FRC:`); a due time more
    than 400 ms ahead is sent at once.
  - VoiceCtl locked (preset load): the tick sends nothing and catches up after.
  - A play-mode switch: the tick forgets what it sent (TE dropped the voices) instead of sending offs.
- **Static check** (chord2_check.py): in chord2_impl.h only `c2_out` (sends only with owner 0), `c2_take_back` (only
  from the owner update, right after owner = 0), `c2_seq_service` (the tick), and `c2_keydown` / `c2_keyup` call TE's
  voice control. The last two do so only for a key that is not a synth keyboard note (TE's own path, as stock), or
  for an untracked key-up with owner 0. Each guard is matched in the source.
  **Dynamic check** (chord2_test.c `t_strum`): every voice call is tagged with its thread. A 6,000-event fuzz (keys,
  HOLD, STRUM and map changes, ticks between) has no KeyHandler call while owned, TE never above 6 notes, and nothing
  left at the end.
- **Timing:** the whole spread is the detent's time (`c2_strum_ms`); a BASS note always starts first.
- **Legato hand-over** (UX round 2): under STRUM, each note a chord change lets go keeps sounding until the next new
  note of the strum starts, one for one in the strum's direction (`off_due`), so a slow roll never thins the pad. The
  tick also caps our sounding notes at 6 before each onset (a voice still handing over goes first), and a new key
  event cuts any hand-over still running. Checked: NEO SOUL with BASS, down 200 ms, legato: never under 4 voices. Note i of n (from the bottom up, or the top down) starts at
  spread x i / (n - 1) (Q8 reciprocals). Common tones of a legato change are not re-struck. Releases are immediate.
  Granularity = one tick (~1.45 ms). Checked: up 30 ms, the first note within a tick of the key, then 10 ms (+- a
  tick) apart, lowest first.
- What still runs on the KeyHandler when strumming: TE's own paths that never were ours (drum mode, a key while
  play mode is not synth), as stock.

### VOICING, bipolar (user request, NIGHT2 follow-up)
- The centre detent is the chord's base voicing: close for a scanned chord, **exactly as played** for a learned one.
- Left (drops) moves notes below the top **down** an octave and **never changes the top note**: DROP 2, DROP 3,
  DROP 2+4, SPREAD (DROP 2+4, then the lowest down again).
- Right (ups) is the mirror and **never changes the bass**: UP 2 (the 2nd note from the bottom up an octave), UP 3,
  UP 2+4, SPREAD UP. The top can change here, by design: "left keeps the melody, right keeps the bass" is one rule to
  learn, and keeping the top on the right too would make UP the same as a TOP change.
- A triad has no 2+4: the knob skips it (the strip shows the gap). Its SPREAD is DROP 2+3 and its SPREAD UP is UP 2+3.
- A two-note chord keeps its bass and moves its upper note: -2..+2 octaves ("-1 OCT", "+1 OCT"). The upper note never
  goes below the bass (skipped). The user's C3 + E4: left = C3 E3 (a 3rd), right = C3 E5 (a 17th).
- Learned chords take TOP and VOICING as transforms of the notes as played. TOP is a ladder on them (each step the
  lowest note up an octave, or the top note down; an interval or a voicing wider than an octave moves whole by octaves), and 0 / 0 is exactly as played. The MVP's TOP on a learned chord
  rewrote its notes; v1 files have TOP 0 there, so nothing changes. VOICING no longer turns a learned chord into a
  scanned one ("TURN GREEN FIRST" is gone).
- A position that does not fit (+-35 from the key) is skipped too. At the end: "WIDEST UP" / "WIDEST DOWN".
- The MIDI 12..115 clamp stays (c2_key_notes).
- **File version 2** stores the position (-4..4) in the byte where v1 had the family (0..3). Version 1 is migrated so
  each chord plays what it played:
  - 4+ notes: CLOSE 0, DROP 2 -1, DROP 3 -2, SPREAD (= DROP 2+4) -3;
  - triads: 0, -1, **-1** (v1's triad DROP 3 played DROP 2), **-4** (v1's triad SPREAD was DROP 2+3);
  - a power chord: any v1 drop (the lower note down an octave) = +1 OCT (the same interval, an octave higher);
  - learned chords: 0.
  Checked chord by chord against the MVP's notes. Saves write version 2. NIGHT2 reads only version 1: going back to
  NIGHT2 after NIGHT3 saved means factory maps there (NIGHT3SAFE is CHORD1 and never reads the file).

### Intervals and wide chords (user feedback 1)
- **Every chord of two pitch classes or fewer is named as an interval by its real distance** (`c2_dyad`): the lowest
  note is the root, the distance runs to the lowest note of the other pitch class. Learned C3 + E4 = "CM10" on the key
  (C over M10), "C MAJOR 10TH" in the title.
- Notation: P perfect, M major, m minor (a lower-case m, UX round 1: "-10" read as a minor chord), A augmented, TT the tritone, 5 the power
  fifth (kept: "C5" is what players say), P8 the octave. Key tags are <= 3 glyphs: m2 M2 m3 M3 P4 TT 5 m6 M6 m7 M7 P8
  m9 M9 m10 M10 P11 A11 P12 m13 M13 m14 M14 P15 ... M21. Titles: "MINOR 2ND" ... "OCTAVE", "MAJOR 10TH", "PERFECT
  12TH", "2 OCTAVES", "MAJOR 21ST". "M" never appears in a chord symbol here (major 7 is the triangle), so "CM10" can't
  be read as a chord. A single learned note: a dot under its name, "E NOTE".
- **Wide learned chords of 3+ pitch classes keep their quality name** ("C" for C3 G3 E4). The VOICING centre then
  says "PLAYED 2 OCT" (span 13..24) or "PLAYED 3 OCT" instead of "AS PLAYED".
- **The WIDE group** (TYPE scan): P8, M10, m10, m7, M7 (root + 7th shells), P12, M9, M13, TT, **left of NOTE**: NOTE, then 1 detent left = the
  octave, 2 = the major 10th. Why there:
  - Every MVP position stays where players learned it (MAJ 1 detent right of NOTE, AUG 28).
  - The everyday 10ths are 2-3 detents from NOTE, not 29 (after AUG).
  - "Right = chords, left = intervals" reads as one idea, and NOTE (one note) sits between.
  - A wall each side of NOTE: a spin either way stops before it.
- Their TOP moves the whole interval by octaves (so it stays the interval it names). Their VOICING moves the upper note
  (and the name follows: M10 at -1 OCT is "CM3"). Appended to the quality enum (codes 29..37), so the file's codes stay.
- Not added: a root-5th-10th shell (it is a major triad in an open voicing: learn it, or MAJ + VOICING).

### The played-notes view (user feedback 2)
- While the page is open, all notes that sound (chords, plain notes, a HOLD pad, the BASS) are lit on the 24 keys and
  named (the other keys keep their chord labels, grey): the top note white, a sounding chord's root blue, the rest light blue; notes off the keyboard as the edge arrows
  with a count. It shows while keys are held and ~0.5 s after the last key-up (the last notes that sounded), then the
  map view comes back. A HOLD pad sounds on: its keys stay dark blue in the map view.
- Priority: a learn (red) > a chord knob turned (~1 s, the edited chord) > the played view > SHAPE's shape > the map.
  It works in every mode (OFF too) and while WRITE auditions.
- The chip on TE's screens: a strum arrow before the chord name when the map strums (UX round 2), otherwise not changed. Listing 4-5 note names there would make it 3x wider than the chord name, over
  the top-right area that was verified clear only at its current size; the chord name already says what sounds.

### WRITE (SHIFT+DROP)
- Gesture: SHIFT+DROP opens WRITE on "FACTORY -> this map" (maps 5..8: "EMPTY -> this map"), so SHIFT+DROP, DROP is
  exactly NIGHT2's factory restore (no second confirm: SHIFT+DROP was the question). Why not another gesture: every
  free key combination on the page is either TE's (SHIFT+T1/T3/T4, the octave keys), already ours (SPLIT, LIFT, DROP,
  SHIFT+SPLIT/LIFT), or unproven (encoder pushes); a write menu that opens on the restore keeps the one map-level
  gesture players learned and extends it.
- Knobs: blue FROM (FACTORY, 7THS, TRIADS, COPY, EMPTY); green WHICH factory map / KEY (the tonic, wraps) / TRANSPOSE
  (-11..+11); white SCALE (MAJOR, MINOR, DORIAN, PHRYGIAN, LYDIAN, MIXOLYDIAN, LOCRIAN, HARM MINOR, MEL MINOR) / OF
  MAP n (the copy's source); orange TO (the target map; "USED" when it is not empty).
- DROP writes (the untouched restore only within ~2.4 s of SHIFT+DROP; WRITE closes after ~10 s idle). After a knob turn, a target that is not empty needs DROP twice ("OVERWRITE MAP n? ] AGAIN", red, ~2.4 s;
  any knob or key in between drops the question). SPLIT or SHIFT+DROP again cancels. T1..T4 / SHIFT+SPLIT close the
  page. One undo gesture (SHIFT+LIFT). The slot keeps playing its own map (mid-set safety): "WRITTEN TO MAP n" says
  where it went. Keys audition the preview (no learn). SHIFT still held after SHIFT+DROP: "LET GO OF SHIFT".
- **FROM OLD MAP n:** whatever the last WRITE replaced (in RAM, until power-off), so a wrong WRITE survives the next
  edit that takes the one undo.
- 7THS / TRIADS: every in-scale key gets the chord stacked in thirds from it (14 keys), the rest play their note; a
  chord outside the list (harmonic minor's C+^7) is kept as notes and named "?". TOPs keep each octave's top notes in
  one register (near A4 on the lower keys, F#5 on the upper, as DIATONIC C). Flats when the scale's parent major has
  flats. The target's BASS / SMOOTH / STRUM stay. Name: "D DORIAN 7", "E& MINOR 3".
- COPY: the same keys, every chord transposed (root + t, kept inside +-35 by octaves; learned chords' notes too), the
  source's settings, the name "II-V-I +2". Capo-style on purpose: a progression laid out on the keys (II-V-I) stays
  where the hands know it.
- FACTORY: any of the 4 into any map (settings and name too). EMPTY: every key plain, settings cleared.
- Map naming: WRITE names what it writes; no free-text naming (12 characters by knob is slow for little gain).
- Checks: every scale x tonic x triads / 7ths gives the scale's chords on exactly the 14 in-scale keys, in register;
  D dorian = D-7 E-7 F^7 G7 A-7 B$7 C^7; harmonic / melodic minor; flats; overwrite confirm; undo; audition; COPY of
  every factory map at every transposition stays playable.

### Diagnostics, line 4
`SEQ TK:<ticks in the last ~1 s> GAP:<longest gap, ms> LIVE|DEAD OWN:<0|1|2> SENT:<notes the tick sent> FRC:<sent by
the stale guard> TO:<takeovers> SR:<SMOOTH resets>`. Line 2's FILE shows the loaded version (`FILE A V1` = migrated).

### Review log (phase 2)
- **UI review** (fresh subagent, TE-style designer, on the C-path renders). Adopted:
  - "USED" on WRITE in grey: a warning is never coral, which reads like red;
  - a wider gap between HOLD and the knob-colour group (`{30 SM B`);
  - interval tags at one size (step 1), and a tighter width on the outlined key so "M10" clears the outline;
  - one-glyph tags ("-", dim) a size up, so they don't read as stray marks;
  - the VOICING strip: the centre tick always drawn above and below the bar, positions this chord has 3 units,
    the ones it doesn't a 1-unit stub (the 9-slot rhythm stays);
  - WRITE: the target digit at the name's size on its baseline, the extra "TO" gone;
  - smaller diagnostic lines (they touched);
  - "{30 MS" with a space;
  - an edge arrow on a lit key moves up beside the black keys;
  - note names on black keys narrower.
  Not adopted: the 2-px outline's narrow neck on G / A (the MVP's shape, unchanged).
- **UX round 1** (fresh subagent, keys player: jazz / neo-soul / house, knows the Cycles). No blockers. Adopted:
  - **WRITE timing:** the untouched SHIFT+DROP, DROP restore only within ~2.4 s of opening (as NIGHT2); later, a used
    map asks OVERWRITE? first; WRITE closes after ~10 s idle (a stray DROP later pastes as usual);
  - **SMOOTH readout:** when SMOOTH played another TOP on the edited key, "SM E4" shows in grey before "TOP Bb4";
    `SM` in the header is grey while the chain starts over, white while it leads on;
  - **a learned 10th's TOP moves it whole by octaves** (as the WIDE quality); a learned voicing wider than an
    octave too ("PLAYED 2 OCT" survives TOP);
  - **minor intervals are a lower-case m** (`~` in the chord font: "Cm10"), so "-10" can't be read as a minor chord;
  - **the played view keeps the map**: unlit keys keep their chord labels (grey), so the next chord stays findable;
  - **STRUM detents** 5 ms steps to 40, 10 to 100, then 120 150 200 250 300 (rolls); a wall at OFF (a spin from up to
    down stops there, ~0.3 s rest to cross); "LOW>HIGH" / "HIGH>LOW" in its message; a BASS note always first;
    "NO SEQ TICK" also when the page opens on a strumming map;
  - VOICING's first right turn after boot says "UP KEEPS THE BASS, DROPS ARE LEFT" (NIGHT2 had drops on the right);
  - the NOTE screen says "GREEN > CHORDS / < INTERVALS"; the WIDE group gains the root + 7th shells (m7, M7) next to
    the 10ths: P8 M10 m10 m7 M7 P12 M9 M13 TT;
  - WRITE outlines the scale's tonic key in the preview.
  Not adopted: delaying a legato change's offs to the new chord's onsets (a hole under long strums; one more state per
  note in the tick: later, if the device shows it); TO in white when empty (orange is the 4th knob; USED is grey).
- **UX round 2** (fresh subagent, live performer: OP-1, Digitakt, Prophet; house, lo-fi, neo-soul). No blockers.
  Adopted:
  - the SMOOTH anchor (white starts from the TOP that sounded, the chord anchored, padlock);
  - an auditioned edit is what SMOOTH leads on from;
  - the legato hand-over under STRUM (re-raised from round 1: a 200 ms roll on a 5-voice pad left a hole), with the
    tick's 6-voice cap;
  - FROM OLD MAP n (a wrong WRITE survives one more edit);
  - SHIFT+white right restarts SMOOTH;
  - the strum arrow on the chip;
  - WRITE keeps the slot on its map; "LET GO OF SHIFT".
  Not adopted: a faster STRUM knob on fast turns (the encoder hook gives 1 step per detent on the page by design:
  hook_steps cap 1); interval tags as "b10" (m / M / P is the standard interval spelling, and the lower-case m can't
  be read as a chord; worth asking players on the device). Undo already re-voices held chords.

