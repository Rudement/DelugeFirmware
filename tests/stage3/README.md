# Stage 3 — velocity offset

Standalone, same reasoning as `tests/stage1` and `tests/stage2`.

`ArpNote` is stubbed rather than included — the real `arpeggiator.h` drags in most of the
firmware — and the effect body is copied verbatim from `velocity_fx.cpp`. **Both go stale if
the originals change.** Only two fields are modelled, checked against `arpeggiator.h:155-184`.

What it pins:

- **Idempotence**, which is why the effect anchors on `baseVelocity` rather than offsetting the
  running value. The seam runs immediately before *every* dispatch, and with the arp off a Sound
  that could not get a voice leaves the note `PENDING` — `handlePendingNotes` then hands the same
  array element back on a later render. Offsetting in place would apply twice on a stolen voice,
  which is the kind of bug that only shows up under load and sounds like nothing in particular.
- **Idle means untouched.** Disabled, or enabled with a zero offset, writes nothing at all. That
  is what keeps the arpeggiator's own velocity spread intact whenever this effect isn't in use.
- **Never velocity 0.** On MIDI that is a note-off, so a note-on at 0 becomes a note-off for a
  note that never started and nothing downstream ever releases it. Floor is 1.
- The whole legal input range exhaustively — every offset against every base velocity.

```
g++ -std=c++20 -Wall -Wextra -o /tmp/t velocity_fx_test.cpp && /tmp/t
```

## Stale, 2026-09-10

VelocityFx itself has not changed, but the context this copies has: `MidiFxContext` now carries a
`NotePool*`, and `flush` takes an instruction and returns whether it wants another. Sync the stubs
before trusting a run.
