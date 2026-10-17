# CHORD v2 and the key-chord map (firmware 246): scope

Status: **design only** (2026-10-16). Nothing is built or flashed; shipped behaviour is unchanged.
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
- **Edited key = the last key pressed.**
  - It is frozen for 2 s after any knob turn, shown by a padlock.
  - While a key is held, the knobs edit that key.
  - Pressing SHIFT ends the freeze, so a learn always goes to the last key tapped.
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
