# MIDI FX, independent of the arpeggiator

Design document. Written 2026-08-09 against `feat/midi-fx-13` (`e3402639`), 1.3 line.
Nothing here is implemented yet.

Target: a MIDI FX stage that processes notes whether or not the arpeggiator is running,
with **note delay / echo** as the first effect. 1.3 first, backport to 1.2.1 once it survives
hardware.

---

## 1. Where things actually stand

`feat/midi-fx-13` created two dispatch seams — `dispatchArpNoteOffs` / `dispatchArpNoteOns`
on `NonAudioInstrument`, and `dispatchArpNoteOffs` on `Sound`. Reading the surrounding code
changes the picture in one important way:

**The note-trigger path is already arp-independent.** `NonAudioInstrument::sendNote`
(`non_audio_instrument.cpp:55`) calls `arpeggiator.noteOn` / `noteOff` unconditionally.
With `mode == ArpMode::OFF`, `Arpeggiator::noteOn` falls through to
`arpeggiator.cpp:375-383`, sets `arp_note->noteCodeOnPostArp[0] = noteCode`, marks it
`PENDING`, and hands it back as `instruction->arpNoteOn`. The seam fires. Same for note-offs
at `arpeggiator.cpp:397`. The name says "Arp"; the path is universal.

So a **note-mapping** effect — transpose, velocity curve, scale quantize, chord — needs no
new plumbing at all. It goes in the seam and works today, arp on or off.

**The time-driven path is not.** Two gates:

| Site | Gate |
|---|---|
| `NonAudioInstrument::renderOutput` (`non_audio_instrument.cpp:37`) | `if (arpSettings.mode != ArpMode::OFF)` — the arp is never even called |
| `ArpeggiatorBase::doTickForward` (`arpeggiator.cpp:1560`) | `if (mode == OFF \|\| syncLevel == 0) return 2147483647;` |
| `ArpeggiatorBase::render` (`arpeggiator.cpp:1484`) | `if (mode == OFF \|\| !hasAnyInputNotesActive()) return;` |

With the arp off, nothing reaches the instrument on a clock. An echo has nowhere to be
called from. **That gap is the work.**

### The precedent that makes this cheap

Both `render` and `doTickForward` already call `handlePendingNotes(settings, instruction)`
*before* the `ArpMode::OFF` check (`arpeggiator.cpp:1481` and `:1555`). That function
(`arpeggiator.cpp:486`) explicitly runs when `arpIsOff(settings)` and starts notes that
couldn't get a voice. So there is already a live, arp-independent path down the time axis,
returning through the same `ArpReturnInstruction`. The MIDI FX tick is the same shape as
something that already works.

### The scheduler is already there

`doTickForwardForArp` returns "ticks until my next event". Callers min it into the global
wake-up:

- `session.cpp:2522` → `nearestArpTickTime = std::min(...)`
- `arrangement.cpp:311` → same

`2147483647` means "never wake me". An effect with a pending echo simply returns a smaller
number. **No new scheduling infrastructure is required** — only permission to return
something when the arp is off.

---

## 2. The paths a MIDI FX has to cover

Four, not one. Getting this wrong is how the feature ends up working on MIDI but not on
synths.

1. **`NonAudioInstrument`** (MIDI + CV) — `sendNote`, `renderOutput`, `doTickForwardForArp`.
   The clean case: both seams exist, output is `noteOnPostArp` / `noteOffPostArp`.

2. **`Sound` — two separate entries.** As the `sound.h:334` comment already warns:
   `Sound::noteOn` (`sound.cpp:1544-1566`) switches notes on **inline**, gated on
   `AudioEngine::allowedToStartVoice()`, leaving the note `PENDING` when no voice is free.
   The other entry is `process_postarp_notes` (`sound.cpp:2367`). An FX stage must sit above
   both, or echoes will fire on one path and not the other.

3. **`SoundInstrument::doTickForwardForArp`** (`sound_instrument.cpp:414`) — the synth's
   clock path, gated identically.

4. **`Kit` / `ArpeggiatorForDrum`** (`kit.cpp:1129`) — a kit-wide arp that maps notes onto
   note rows. Note that `ArpeggiatorForDrum` writes only slot 0
   (`arpeggiator.cpp:873`); chord polyphony is nullified at `kit.cpp:1149-1151`.
   **Recommend: out of scope for v1.** Note the exclusion explicitly rather than discovering
   it later.

---

## 3. The hard problem: identity for generated notes

This is the part that decides the architecture, and it is not obvious from the seam.

`noteOnPostArp(noteCodePostArp, ArpNote* arpNote, noteIndex)` takes a **pointer to an
`ArpNote` living in the arpeggiator's `notes` array**, and it *writes back into it*:
`arpNote->outputMemberChannel[noteIndex] = outputMemberChannel`
(`midi_instrument.cpp:828`, `cv_instrument.cpp:36`). Two consequences:

**(a) MPE channel allocation surveys that array.** `MIDIInstrument::noteOnPostArp`
(`midi_instrument.cpp:846-866`) walks `arpeggiator.notes` counting notes per member channel
to pick the least-crowded one. A generated echo note that is not in that array is **invisible
to the survey**, so live notes will be assigned channels already in use by ringing echoes.
On a non-MPE output this doesn't matter; on MPE it produces wrong pitch bend on the wrong
notes.

**(b) The source `ArpNote` is destroyed on release.** `Arpeggiator::noteOff` calls
`notes.deleteAtIndex(notesKey)` (`arpeggiator.cpp:432`). An echo that outlives the held
note — which is the entire point of an echo — cannot hold a pointer into that array. It
would dangle, and `ResizeableArray` moves elements on delete, so it would dangle *silently*.

Also `ARP_MAX_INSTRUCTION_NOTES = 4` (`arpeggiator.h:37`), so an `ArpNote` cannot itself
carry more than four simultaneous output notes. Not enough for an echo tail.

### Options

| | Approach | Verdict |
|---|---|---|
| A | Reuse the source `ArpNote` for its echoes | **No.** Dangles on release; 4-slot ceiling. |
| B | FX owns a fixed pool of `ArpNote` records; extend the MPE survey to iterate the pool as well as `arpeggiator.notes` | **Recommended.** Bounded memory, no allocation in the audio path, correct MPE. Cost: one extra loop in `noteOnPostArp`, and the survey has to become a callback or an iterator rather than a raw loop over `notes`. |
| C | Echoes reuse the parent's output member channel; no independent allocation | Cheap, but on MPE all repeats share one channel and inherit the parent's bend. Acceptable *fallback* if B proves invasive — degrade only when `sendsToMPE()`. |

**Recommendation: B, with C as the documented degradation when the pool is exhausted.** Size
the pool statically (32 entries is 16 sixteenth-note repeats across two held notes, and
`ArpNote` is small — two `int16_t`, an MPE array, two `uint8_t`, and three 4-element arrays).
No `GeneralMemoryAllocator` use on the note path.

---

## 4. Architecture

```
                    input note (live / sequencer)
                              │
                              ▼
                       Arpeggiator                     ← unchanged
                    (pass-through when OFF)
                              │
                     ArpReturnInstruction
                              │
                              ▼
              ┌───────── MidiFxChain ─────────┐        ← NEW
              │  transform(instruction)       │        note-mapping, in-place
              │  schedule(events)             │        time-based, into FX queue
              └───────────────┬───────────────┘
                              │
                              ▼
              dispatchArpNoteOns / dispatchArpNoteOffs  ← existing seam
                              │
                              ▼
                  noteOnPostArp / noteOffPostArp
                     (MIDI / CV / Sound voice)
```

Plus a second entry into the chain from the clock:

```
  session.cpp / arrangement.cpp
        doTickForwardForArp(pos)
              │
              ├─→ arp tick        (as today, gated on mode != OFF)
              └─→ MidiFxChain::tick(pos)   ← NEW, NOT gated on arp mode
                        │
                        └─→ due events → same dispatch seam
                        └─→ returns ticks til next FX event, min'd with the arp's
```

### Interface sketch

```cpp
class MidiFx {
public:
    virtual ~MidiFx() = default;

    /// Called for every note event, arp on or off. May rewrite the instruction in place,
    /// and may schedule future events. Returns false to swallow the event entirely.
    virtual bool onNoteEvent(MidiFxContext&, ArpReturnInstruction&) = 0;

    /// Called on every clock tick regardless of ArpMode. Emits due events into the
    /// instruction. Returns ticks until its next event, or 2147483647 for none.
    virtual int32_t tick(MidiFxContext&, uint32_t clipPos, ArpReturnInstruction&) = 0;

    /// All notes off — clip stop, transport stop, effect disabled, param change that
    /// invalidates the queue.
    virtual void flush(MidiFxContext&) = 0;
};
```

`MidiFxContext` carries what the effect needs without dragging in `ModelStack`: the note
pool, the sync/tick conversion, and whether the output is MPE.

### Two clock modes

The arp has both a **synced** path (`doTickForward`, quantised to
`ticksPerPeriod = 3 << (9 - syncLevel)`, `arpeggiator.cpp:1563`) and a **free-running** path
(`render`, driven by `phaseIncrement` from `getPhaseIncrement`, `arpeggiator.cpp:1592`).

**v1: synced only.** It reuses the tick scheduler, it is what people want from a note delay,
and it avoids a second timing implementation. Free-running (delay time in ms, usable with
the transport stopped) is a follow-on. Note the consequence: **with the transport stopped,
v1 echoes do not sound.** That is a real limitation and should be stated in the menu docs,
not discovered by a user.

---

## 5. Note delay / echo — behaviour

| Param | Range | Notes |
|---|---|---|
| Time | sync divisions, reuse `SyncLevel` + `SyncType` from `model/sync.h` | Even / triplet / dotted come free |
| Repeats | 0–16, plus "infinite" at the top | Infinite must still be bounded by the pool |
| Feedback (velocity decay) | 0–100% per repeat | Below a velocity floor, drop the note rather than emit velocity 0 |
| Pitch shift per repeat | −24..+24 semitones | 0 = plain echo; non-zero gives the classic rising/falling delay |
| Gate / repeat length | fraction of the division | Determines when the paired note-off is scheduled |

Behaviours to decide explicitly, because guessing wrong is what makes an echo feel broken:

- **Note-off pairing.** Every scheduled note-on schedules its own note-off. Repeats do
  *not* wait for the parent's release. Otherwise a staccato input yields infinite sustain.
- **Release mid-tail.** Releasing the held key must **not** kill the tail — that is the
  whole point. Requires the pool of §3, not a pointer into `arpeggiator.notes`.
- **Transport stop / clip stop.** `flush()` — hard note-off for everything in the pool. Any
  path that can leave a MIDI note hanging is a bug report from a user with an external synth.
- **Param change while ringing.** Changing Time mid-tail: recompute pending event times, do
  not flush. Changing Repeats down: truncate. Both cheap, both surprising if wrong.
- **Arp + echo together.** Once the gates come off, both run. Arp output feeds the echo.
  Worth capping total events per tick so a fast arp into a long echo cannot flood
  `dispatchArpNoteOns`.

---

## 6. Params, menu, persistence

Existing pattern to copy, all verified:

- **Params:** `UnpatchedShared` in `modulation/params/param.h:206-224`. The arp block sits
  at the end, terminated by `UNPATCHED_LAST_ARP_PARAM` / `UNPATCHED_NUM_SHARED`, and
  `UnpatchedSound` / `UnpatchedGlobal` continue from there.
- **File compatibility:** params serialize **by name**, not index —
  `paramNameForFileConst` (`param.cpp:425`), read back by string comparison at
  `param.cpp:783`. So adding a MIDI FX block after `UNPATCHED_SPREAD_VELOCITY` renumbers the
  enum without breaking saved songs. This is a genuinely favourable finding; confirm it
  holds for the MIDI-follow file too (`forMidiFollowFile`).
- **Menus:** `gui/menu_item/arpeggiator/` is the template — `arp_unpatched_param.h`,
  `sync.h`, `mode.h`. A parallel `gui/menu_item/midi_fx/` folder.
- **Settings + XML:** mirror `ArpeggiatorSettings::writeCommonParamsToFile` /
  `readCommonTagsFromFile` (`arpeggiator.cpp:1847`), called from `instrument_clip.cpp:2380`
  and `:2653`. Per-clip, exactly like `arpSettings`, plus a `defaultMidiFxSettings` on the
  Instrument to match `defaultArpSettings` (`instrument_clip.cpp:2286`).

---

## 7. Staged plan

Each stage builds and is independently reviewable. Stages 1 and 2 are separately
upstreamable — worth keeping them that way given `feat/midi-fx-13` is already the most
upstream-ready branch in the set.

**Stage 1 — decouple the clock from `ArpMode`.** No effects. Restructure the three gates so
the time-driven paths are entered regardless of arp mode, with the arp's own work still
gated internally (the `handlePendingNotes` pattern, generalised). Add a no-op FX hook that
returns `2147483647`. Success: identical behaviour on hardware, verified by ear and by the
existing tests. *Small, mechanical, and the natural upstream companion to the seam commits.*

**Stage 2 — `MidiFx` interface + note pool.** The interface from §4, the pool from §3, the
MPE survey extension in `midi_instrument.cpp`, `flush()` wired to transport stop and clip
stop. Ship with a null effect. Success: still nothing audible changes.

**Stage 3 — note delay/echo, hardcoded params.** No menu yet; fixed division, 3 repeats,
75% feedback, built into a test binary. **This is the first listening test**, and the point
at which the design either holds up or doesn't. Test on MIDI out first (easiest to verify
against a scope or a DAW), then CV, then a synth.

**Stage 4 — params, menu, persistence.** §6. Success: survives save/load, and an old song
loads unchanged.

**Stage 5 — 1.2.1 backport.** See §8.

**Stage 6 — hardware pass + upstream.** Format-check first (`dbt format`); per HANDOFF
nothing on this line has ever been format-checked.

---

## 8. The 1.2.1 backport

Not a cherry-pick. `ArpReturnInstruction` on 1.2.1 is still **single-note** — scalar
`noteCodeOffPostArp` / `noteCodeOnPostArp`, no `glide*` arrays, no `invertReversed`, and
`ArpNote` has no `noteStatus` / `baseVelocity`. The 1.3 seam commits assume all of it.

Order for 1.2.1:

1. Backport the multi-note `ArpReturnInstruction` / `ArpNote` — or write a 1.2.1-native
   single-note variant of the seam. The second is smaller but forks the code.
2. Backport the two seam commits (`5385f59b`, `e3402639`).
3. Then Stages 1–4.

Decide 1 vs. the fork before starting; it is the whole cost of the backport. Given 1.2.1 is
the release line, the single-note variant is probably right — less churn on the branch users
actually flash.

---

## 9. Risks

| Risk | Mitigation |
|---|---|
| **Hanging MIDI notes.** Every escape path — transport stop, clip stop, song swap, effect disabled, param change, pool exhaustion — must flush. | Enumerate the paths in Stage 2 and test each deliberately. Treat as the acceptance criterion for the stage. |
| **MPE channel corruption** from echoes invisible to the survey. | §3 option B. If B proves too invasive, degrade to C *only* when `sendsToMPE()`. |
| **Audio-thread cost.** `renderOutput` runs per audio block. | No allocation on the note path; fixed pool; cap events per tick. |
| **Behaviour change with the arp off** from restructuring the gates. Stage 1 touches code every note passes through. | Stage 1 ships with zero functional change and is verified by ear before anything is built on it. |
| **Kit arp interaction** — `ArpeggiatorForDrum` uses slot 0 only and nullifies chord params. | Out of scope for v1, stated up front. |
| **Sound's two note-on entries** diverging. | §2 item 2. Add an assertion or a shared helper rather than trusting both call sites to stay in step. |

## 10. Open questions

1. **Per-clip or per-instrument?** `arpSettings` is per-clip with an instrument-level default.
   Same for MIDI FX, or instrument-only? Per-clip is more consistent and more useful, and
   costs nothing extra given the pattern already exists.
2. **Does the echo feed the sequencer's record path?** If someone records while echoing,
   do repeats land as notes? Almost certainly no — but it needs deciding, not defaulting.
3. **Pool size.** 32 is a guess. Worth deriving from worst case: max repeats × polyphony.
4. **Free-running mode.** Deferred, but if delay-with-transport-stopped is important to you,
   it changes Stage 1 — the `render` path would need the same treatment as `doTickForward`,
   not just a pass-through.

---

## Appendix — key references

| What | Where (1.3, `e3402639`) |
|---|---|
| Dispatch seams | `non_audio_instrument.cpp:153,168`; `sound.cpp:1596` |
| Arp pass-through when OFF | `arpeggiator.cpp:375-383` (on), `:397` (off) |
| Arp-independent tick precedent | `handlePendingNotes`, `arpeggiator.cpp:486` |
| The three gates | `non_audio_instrument.cpp:37`; `arpeggiator.cpp:1484,1560` |
| Tick scheduler | `session.cpp:2522`; `arrangement.cpp:311` |
| Sound's inline voice-gated path | `sound.cpp:1544-1566` |
| Sound's other note-on entry | `process_postarp_notes`, `sound.cpp:2367` |
| `ArpNote` / `ArpReturnInstruction` | `arpeggiator.h` (`ARP_MAX_INSTRUCTION_NOTES = 4`, line 37) |
| MPE member channel survey | `midi_instrument.cpp:846-866` |
| Param enum + by-name serialization | `param.h:206-224`; `param.cpp:425,783` |
| Arp settings XML | `arpeggiator.cpp:1847`; `instrument_clip.cpp:2380,2653` |
