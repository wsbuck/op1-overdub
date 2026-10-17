"""Gallery captions for design/chord2/render.py (scene -> (title, text)). Design: notes/chord2.md."""
CAPTIONS = {
    "map_play": ("MAP: playing", "DIATONIC C, G4 held: name big (blue underline = root knob, green = quality knob), "
                 "notes on the ruler (root blue, top white), key lit dark blue, outlined = edited; padlock = frozen."),
    "map_edit_top": ("MAP: TOP", "C4 edited, white turned two steps: G on top -> E5 on top (TOP readout bright)."),
    "map_edit_voicing": ("MAP: VOICING", "Orange: DROP 2. The top note stays where TOP put it."),
    "frozen_playing": ("Edit while playing", "C4 frozen for editing (padlock) while G7 is played: the sounding chord "
                       "shows light blue at the right."),
    "learned_slash": ("Slash chord kept", "Learned B D G on C4 = G/B; turning TYPE to ^7 keeps the B bass: G^7/B."),
    "map_shift": ("SHIFT layer", "SHIFT held: the map's settings on the knobs: MODE, STRUM (whole spread), SMOOTH, BASS."),
    "learn": ("Learn", "C4 edited, SHIFT + A C E G: named live in red; letting go of SHIFT writes the notes still held."),
    "learned": ("Learned", "C4 = A-7 as played; white confirmation; SHIFT+LIFT undoes."),
    "map_251": ("Factory II-V-I", "ii-V-I in F, Bb, Eb, C on groups of three white keys, D D# E = minor ii-V-i; SMOOTH."),
    "map_neo": ("Factory NEO SOUL", "m9 / maj9 / 13 colours, flat-root chords on the black keys; BASS, SMOOTH, 20 ms "
                "strum, HOLD."),
    "map_pop": ("Factory POP", "C major triads on the white keys, borrowed chords on the black keys."),
    "write_scale": ("WRITE: scale", "SHIFT+DROP: F major sevenths into empty MAP 5; keys audition the preview."),
    "write_copy": ("WRITE: copy", "MAP 2 up a whole step into MAP 6 (a song in another key)."),
    "write_confirm": ("WRITE: overwrite", "Factory NEO SOUL over a used MAP 1: red, DROP again to confirm."),
    "shape": ("SHAPE", "CHORD1's parallel shape with TOP and VOICING: -7 drop 2 from C4; lit keys = its notes."),
    "unmapped": ("Unmapped key", "C#4 has no chord: it plays its note; turning green gives it one."),
    "cleared": ("Cleared", "SPLIT + F4 cleared it; SHIFT+LIFT undoes."),
    "badge": ("Badge, our engines", "Replaces CHORD1's box: map number and the last chord (blue frame = MAP)."),
    "badge_seq": ("Badge, sequencer on", "The arp / a sequencer takes the keys: chords bypassed, and the badge says so."),
    "chip": ("Chip, TE screens", "Frame hook (probe 1): the sounding chord, top right, black backing, over any screen."),
    "chip_idle": ("Chip, silent", "Nothing sounding but the slot is in MAP 3: the mode, dim."),
}
