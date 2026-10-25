# Stage 4 — gate time

Standalone, same reasoning as the other stage tests. The arpeggiator types are stubbed and the
three method bodies are copied **verbatim** from `gate_time_fx.cpp` with the `GateTimeFx::`
qualifier stripped. If the original changes this copy goes stale — same hazard as `tests/stage1`.

This is the first effect that schedules something for later, so it is the first that can be
quietly wrong in a way you would not hear until a note stuck. What it pins:

- **Inert cases schedule nothing** — disabled, 100%, arp on, or a live note with no length.
- **The basic case**: length 96 at 50% releases 48 ticks later, on the right channel.
- **Never clobbers the arp's note-offs.** `tick()` writes into the first *free* slot of the
  instruction, not slot 0. The first draft wrote from 0 and would have stranded arpeggiated
  notes whenever a gate came due on the same tick.
- **More due than the instruction can carry** returns 0 to be actioned again rather than
  dropping the remainder. Four is the whole capacity of an `ArpReturnInstruction`.
- **A rewind releases everything.** A clip loop moves the position backwards; the note is about
  to be retriggered or has already been cut, so let it go rather than strand the note-off.
- **Overflow drops the newest note's gate**, never an existing note-off. A full-length note
  sounds wrong; a note-off that never fires sticks.
- **The gate never rounds to zero ticks**, checked across every percentage against every length
  from 1 to 200, along with "shortens only, never extends".

The first run of this test caught a real bug: with no previous position `tick()` treated the
situation as a discontinuity and fired everything, so the first clock tick after a note was
scheduled released it instantly. No previous position means *no time has passed*, not *jump*.

```
g++ -std=c++20 -Wall -Wextra -o /tmp/t gate_time_fx_test.cpp && /tmp/t
```

## Stale, 2026-09-10

This test copies GateTimeFx's implementation verbatim, and that implementation has since changed in
ways the copy does not know about:

- Gate Time is a ratio in both directions now, 5-400%, not shorten-only. Over 100% the note is held
  past its written end and the sequencer's own note-off is swallowed.
- An extension shorter than `kMinExtensionTicks` is not an extension. Hardware found this the hard
  way: at 101% a sixteenth note computes to the same tick count as at 100%, and taking the note over
  anyway put the release on the same tick as the next note.
- A pending release is dropped when the same note code starts again in the instruction, because the
  two notes are indistinguishable at the output.
- `flush` takes an instruction, emits releases for notes held past their written end, and returns
  whether it ran out of note-off slots.

The copy needs regenerating from the real file before a run means anything. It is not in the
firmware build, so nothing depends on it in the meantime.
