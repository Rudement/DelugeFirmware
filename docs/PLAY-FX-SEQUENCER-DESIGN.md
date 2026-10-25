# Clock Shift and Beat Stretch: the two Play FX that are not note effects

Design note, written 2026-09-09 against `feat/midi-fx-rs7000-13` (`09ae59ed`), 1.3 line.
Nothing here is implemented yet.

Of the RS7000's Play FX set, Velocity, Gate Time and MIDI Delay are now built. The remaining
two are a different kind of thing, and pretending otherwise is how this would go wrong.

## 1. Why these two cannot live where the others do

MIDI FX sits at the note-event seam: a note arrives, an effect rewrites it or schedules
something against it. That works because everything those effects need is in front of them —
the note, its velocity, its length.

Clock Shift and Beat Stretch need what is *behind* them.

- **Clock Shift** moves the phrase earlier or later by a number of ticks. Later is expressible
  at the seam (hold the note back). **Earlier is not** — a note cannot be emitted before the
  seam has been told about it. Half a control is worse than none.
- **Beat Stretch** plays the phrase at a different rate against the clip's length. The seam
  never sees a rate; it sees notes at the moment the sequencer decides to play them. The
  decision is the thing being changed.

Both act on the reading of the sequence, so both belong to the sequencer.

## 2. The two places that matter

The read path narrows to one accessor and one return value, which is what makes this tractable
at all.

**Position in.** `ModelStackWithNoteRow::getLastProcessedPos()` (`model_stack.cpp:89`) is where
every note row learns where it is — `NoteRow::processCurrentPos` takes its `effectiveCurrentPos`
from it (`note_row.cpp:2154`), and the note search, the note-off bookkeeping and the
ticks-til-next arithmetic all derive from that one value. It already has two branches, for
independent and clip-driven play positions, so a transform applied there covers both.

    shifted  = pos + clockShiftTicks                     (wrapped into 0..length)
    stretched = pos * 100 / beatStretchPercent           (wrapped into 0..length)

**Ticks out.** `NoteRow::processCurrentPos` returns "ticks until my next event", which
`InstrumentClip::processCurrentPos` mins across rows (`instrument_clip.cpp:731`). Under Beat
Stretch that number is computed in the stretched frame and consumed as real ticks, so it must be
scaled back on the way out or every wake-up lands at the wrong time. Clock Shift needs no such
scaling — a constant offset does not change intervals.

That asymmetry is the whole reason to build Clock Shift first: it touches one accessor, and its
correctness argument is short.

## 3. What will bite

Each of these is a decision, not a detail. Getting one wrong produces a stuck note or a note
row that silently stops playing, and both look like "the firmware broke" rather than "the effect
is wrong".

1. **The loop point.** `NoteRow::processCurrentPos` decides it has reached the end by comparing
   its own `lastProcessedPosIfIndependent` against `effectiveLength` — raw members, not the
   transformed value. Transform the read and those two frames disagree at the boundary. The
   likely answer is that end-of-loop detection stays in the untransformed frame and only note
   lookup moves, but that has to be established rather than assumed.
2. **Recording.** A note recorded while Play FX is on would land at the transformed position,
   baking the effect into the phrase. On the RS7000 Play FX is non-destructive until you
   normalize it, so recording should almost certainly capture the *untransformed* position.
   `ignoreNoteOnsBefore_` and the quantise path both read positions and need checking against
   this.
3. **Reversed and pingpong playback.** `effectiveForwardPos` is already a second frame
   (`note_row.cpp:2155`). Adding a third without deciding the order of operations is how a
   reversed clip ends up shifted the wrong way.
4. **Independent note-row play positions and lengths.** A row with its own length wraps at a
   different point from the clip. Clock Shift wrapping has to use the row's effective length,
   which is why the transform belongs in the model-stack accessor that already knows which one
   applies.
5. **Automation.** `ParamManager::processCurrentPos` is driven by `ticksSinceLast` on the same
   path. Under Beat Stretch, either automation stretches with the notes or it does not — a real
   musical choice. Stretching with them is the less surprising answer.
6. **Where a stretch of 100% and a shift of 0 must cost nothing.** Both transforms sit on the
   hottest path in the sequencer. Both need an early return that is provably identical to
   today's behaviour, and that is the acceptance test for the first stage.

## 4. Ranges and storage

Per clip, next to `midiFxSettings`, saved by attribute name the same way:

| Control | Range | Off value |
|---|---|---|
| Clock Shift | −192..+192 ticks (a bar either way at the default resolution) | 0 |
| Beat Stretch | 25%..400% | 100 |

Both belong on the MIDI FX page group as a fifth page, named Play FX rather than added to an
existing page — they are clip-wide, not per-note, and putting them beside Velocity would imply
they behave like it.

## 5. Staging

**Stage A — Clock Shift, positive and negative.** One accessor, one setting, one menu item.
Acceptance: a clip with shift 0 is bit-identical in behaviour; a shift of one sixteenth moves
the phrase audibly and the clip still loops cleanly; no stuck notes across the loop point.

**Stage B — Beat Stretch.** The accessor transform plus the ticks-out scaling. Acceptance:
100% is identical to today; 50% plays the phrase twice per loop; 200% plays half of it. Then the
awkward one — what happens at the loop point when the stretched phrase does not divide evenly
into the clip length. RS7000 truncates. Truncating is also the cheap answer, and matching the
reference is a good reason to pick it.

**Stage C — Normalize.** RS7000 Play FX can be baked into the phrase. Worth having, because a
non-destructive effect you cannot commit is a thing you have to remember forever. Out of scope
until A and B are heard on hardware.
