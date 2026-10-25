# MIDI FX, independent of the arpeggiator

Design document. Written 2026-08-09 against `feat/midi-fx-13` (`e3402639`), 1.3 line.

**Done, built and heard on hardware.** The line is now `feat/midi-fx-rs7000-13`; the RS7000 Play FX
set is complete — Velocity, Gate Time, MIDI Delay, Clock Shift, Beat Stretch — plus a free-running
Retrigger that is not theirs. What hardware changed about the design is in the postscript at the
end; read that before trusting any paragraph below it, because several of them were written from
reading code and hardware disagreed.

Target: a MIDI FX **page**, sibling to the Arpeggiator, whose effects process notes whether or
not the arpeggiator is running. Modelled on the Yamaha RS7000's Play FX. Three effects in v1 —
Velocity, Gate Time, note delay / echo — one page each, built cheapest-first so the page has
content and hardware verification early. 1.3 first, backport to 1.2.1 once it survives
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

> ### Correction, from building Stage 1
>
> The table above overstates the problem. Only **one** of those gates actually blocked the
> time axis, and Stage 1 was correspondingly smaller than this section implies.
>
> | Path | What building it found |
> |---|---|
> | `NonAudioInstrument::renderOutput` | **The only real gate.** Everything, dispatch included, sat inside `mode != ArpMode::OFF`. MIDI and CV genuinely had no free-running clock without the arp. |
> | `Sound::render` (`sound.cpp:2472`) | **Already arp-independent.** It has an `else` branch calling `handlePendingNotes`, and it dispatches unconditionally. This is the pattern; the MIDI/CV side was restructured to match it. |
> | `NonAudioInstrument::doTickForwardForArp` | **Already unconditional.** No instrument-level gate at all. |
> | `SoundInstrument::doTickForwardForArp` | **Already unconditional.** Same. |
> | `ArpeggiatorBase::doTickForward` / `render` | Internal gates, and `handlePendingNotes` runs *before* both. They return the sentinel rather than blocking the caller, so nothing needed opening. |
>
> Also confirmed: `doTickForwardForArp` is called for **every** `Output`, unconditionally, from
> both `session.cpp:2522` and `arrangement.cpp:311`. The synced tick path was live all along.
> §1's "the scheduler is already there" was right, and understated.
>
> **One behaviour difference was found and deliberately left alone.** `Sound::render` calls
> `handlePendingNotes` when the arp is off, which starts a note left `PENDING` by switching the
> arp off mid-hold (see the "sticky on purpose" comment at `arpeggiator.cpp:270`). MIDI and CV
> have no voice allocation and have never done this, so a held note goes silent there instead.
> Making them agree is defensible and is a real behaviour change, so it is not in a stage whose
> contract is that nothing changes. **Open question for Stage 2**, added to §10.

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

## 5. The effects

Three, in the order they should be built. The ordering is not the order of interest — it is
cheapest-first, so the page has something real on it, and hardware, early.

Named against the RS7000's Play FX, which is the reference: Beat Stretch, Clock Shift, Gate
Time, Velocity, MIDI Delay. Three of those five map onto this seam. **Clock Shift and Beat
Stretch do not** — they operate on the sequenced phrase, not on note events, and belong to the
sequencer if they are ever wanted. Say so out loud rather than leaving them as implied scope.

### 5a. Velocity — no clock, no pool, works in the seam today

Offset and/or scale on the way through. `sendNote` already runs every note through the
arpeggiator unconditionally and the arp passes through when `mode == OFF` (§1), so a
note-mapping effect needs **no new plumbing at all** — not even Stage 1's. It is the cheapest
possible first effect and the only one that could have shipped before any of this work.

| Param | Range | Notes |
|---|---|---|
| Amount | −64..+63 offset, or 0–200% scale | Pick one. Offset is more predictable; scale preserves dynamics. Offset recommended. |

Clamp to 1..127, not 0..127 — velocity 0 is note-off on MIDI, and emitting it would be a
hanging-note bug wearing a disguise.

**Interaction with arp velocity spread, as built.** Anchoring on `baseVelocity` for idempotence
means a non-zero offset *overrides* the arp's spread rather than stacking with it — the spread
lives in the gap between `baseVelocity` and `velocity`, and anchoring on the base discards it.
At zero offset, and when disabled, the effect returns early and writes nothing, so spread is
untouched whenever this is idle. Stacking the two wants a deliberate decision about which
should win, not an accident of ordering; open question 9.

### 5b. Gate Time — needs the clock, does not need the pool

**This is less cheap than it looks, and the reason is worth reading before scoping it.**

On the RS7000, Gate Time is a percentage because it acts on a *sequenced* phrase where every
note's length is known. At this seam that is only half true:

- **Sequenced notes carry their length.** `NoteRow` passes `sampleSyncLength = thisNote->getLength()`
  into the pending note-on (`note_row.cpp:2558`), and it reaches `sendNote`. A true percentage
  works.
- **Live notes do not.** A key held on a controller has no length until it is released. A
  percentage is undefined. The only honest options are a fixed absolute gate (force note-off
  after N ticks) or pass-through.

So Gate Time is really two behaviours behind one control. Recommend: **percentage for
sequenced notes, pass-through for live ones in v1**, and say so in the menu docs. A fixed
absolute gate for live notes is a reasonable follow-on but is a different control and probably
deserves a different name.

Scope constraint that keeps it out of the pool: **v1 shortens only, never extends** (5–100%).
A shortened note-off always lands while the parent `ArpNote` is still alive, so it needs
identity but not a pool — kept by value, never as a pointer into `arpeggiator.notes`, which
moves its elements on delete. Anything over 100% outlives the parent and drags in all of §3.

~~The one piece of bookkeeping it does need: having emitted an early note-off, the *real*
note-off must be swallowed rather than sent twice.~~ **Not needed** — see Stage 4 in §7. A
duplicate note-off is harmless on all three outputs, and suppressing it is worse than allowing
it.

### 5c. Note delay / echo — the one that needs everything

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

## 6. Params, page, persistence

**Decision: MIDI FX gets its own page, one page per effect, sibling to the Arpeggiator — not a
submenu inside it.** The whole premise of the feature is that it is arp-independent, and
burying it under the arp would say the opposite in the only place a user actually looks.

### The page

1.3 already has the machinery, and the arp is the worked example. `HorizontalMenu`
(`gui/menu_item/horizontal_menu.h`) is a named page of parameters with paging;
`HorizontalMenuGroup` (`horizontal_menu_group.h`) chains several pages into one navigable
strip; a plain `Submenu` holds the lot and hangs off the sound editor root. The arp uses
exactly this shape — `arpBasicMenu` and `arpPatternMenu` grouped into `arpMenuGroup`, wrapped
in `arpMenu` (`gui/ui/menus.cpp:363-404`).

Mirror it:

```
Submenu midiFxMenu {
    &midiFxEnabledMenu,       // one switch, like arpModeMenu
    &midiFxVelocityMenu,      // HorizontalMenu - 5a
    &midiFxGateMenu,          // HorizontalMenu - 5b
    &midiFxDelayMenu,         // HorizontalMenu - 5c
}
HorizontalMenuGroup midiFxMenuGroup {{&midiFxVelocityMenu, &midiFxGateMenu, &midiFxDelayMenu}};
```

A page per effect, not one shared page, because a `HorizontalMenu` shows four items at a time
and **Delay alone wants four** — Time, Repeats, Feedback, Pitch. One shared page would cap the
feature at roughly one effect. Per-effect pages also mean adding an effect later is one more
page rather than a re-layout.

`gui/menu_item/midi_fx/` parallels `gui/menu_item/arpeggiator/`; `arp_unpatched_param.h`,
`sync.h` and `mode.h` are the templates to copy.

### Where it attaches

`midiFxMenu` goes next to `&arpMenu` / `&arpMenuMIDIOrCV` in each sound editor root. There are
more of these than is obvious, and missing one is how a feature ends up working on synths but
not on MIDI:

| Root | `menus.cpp` |
|---|---|
| `soundEditorRootMenu` (synth) | `:1477` |
| `soundEditorRootMenuMIDIOrCV` | `:1655` |

> **Corrected while building Stage 3: two roots, not five.** The other three —
> `soundEditorRootMenuDrum`, `...MidiDrum`, `...GateDrum` — are all Kit rows, and Kits are out
> of scope for v1. `SoundEditor::setup` sets `currentMidiFxSettings` only in the non-Kit branch,
> leaving it null on Kits, and every menu item's `isRelevant()` checks it. So the pages are
> **hidden** on Kits rather than shown and inert, which is the behaviour you want anyway.

**And the roots are not the page.** `midiFxMenuGroup` must also go in
`horizontalMenusChainForSound` and `horizontalMenusChainForMidiOrCv` (`menus.cpp:1975-2012`) —
*that* is what puts MIDI FX in the strip of pages you cycle through, next to Arp and Randomizer.
The `Submenu` alone is only reachable by navigating in. Missing this would leave the group as
dead code and the feature buried, which is exactly what having its own page was meant to avoid.

### Params

- **Enum:** `UnpatchedShared` in `modulation/params/param.h:183-225`. The arp block runs from
  `UNPATCHED_FIRST_ARP_PARAM` to `UNPATCHED_LAST_ARP_PARAM`, and `UNPATCHED_NUM_SHARED` is
  *defined as* `UNPATCHED_LAST_ARP_PARAM`, with `UnpatchedSound` and `UnpatchedGlobal`
  continuing from it. A MIDI FX block goes after the arp block and before `NUM_SHARED`, which
  renumbers both of those enums.
- **That renumbering is safe** because params serialize **by name**: `paramNameForFileConst`
  (`param.cpp:425`), read back by string comparison (`param.cpp:783`). Saved songs are keyed on
  strings, not indices. Still confirm it holds for the MIDI-follow file (`forMidiFollowFile`)
  and for anything that persists a param index rather than a name — automation and the mod
  matrix are the places to check.
- **Cost, which is not free:** every shared unpatched param is an `AutoParam` on *every*
  `ParamManager`, including ones that will never use MIDI FX. Adding six or eight params to
  `UnpatchedShared` is a per-clip RAM cost on a device that is already tight. Worth measuring
  before settling the param count, and worth asking whether the delay's params want to be
  plain settings rather than automatable unpatched params. **Automatable is the more useful
  answer and the more expensive one — decide deliberately.**

### Persistence

Mirror `ArpeggiatorSettings::writeCommonParamsToFile` / `readCommonTagsFromFile`
(`arpeggiator.cpp:1847`), called from `instrument_clip.cpp:2380` and `:2653`. Per-clip, like
`arpSettings`, plus a `defaultMidiFxSettings` on the Instrument to match `defaultArpSettings`
(`instrument_clip.cpp:2286`).

Acceptance test for this stage is the boring one and it is the one that matters: **a song saved
before MIDI FX existed loads unchanged, and a song saved with MIDI FX loads on a build that has
it.** Test both directions.

---

## 7. Staged plan

Each stage builds and is independently reviewable. Stages 1 and 2 are separately
upstreamable — worth keeping them that way given `feat/midi-fx-13` is already the most
upstream-ready branch in the set.

**Stage 1 — decouple the clock from `ArpMode`. DONE**, on `feat/midi-fx-stage1-13`
(`a2f09379`). One gate restructured, not three — see the correction in §1. Adds
`modulation/midi_fx/midi_fx.h`: `render(numSamples, instruction)` on the free-running axis and
`tick(clipPos, instruction)` on the synced one, both inline no-ops, `tick` returning
`kNoEvent`. Both instruments now `std::min` that into what `doTickForwardForArp` returns.
Four call sites: `NonAudioInstrument::renderOutput` and `::doTickForwardForArp`,
`Sound::render`, `SoundInstrument::doTickForwardForArp`.

The zero-change claim rests on a default-constructed `ArpReturnInstruction` being inert — the
constructor fills every note slot with `ARP_NOTE_NONE` and nulls `arpNoteOn`, and both dispatch
loops break on the first such slot. So the arp-off path now runs three calls that do nothing.
That specific claim is checked mechanically by `tests/stage1/inert_instruction_test.cpp`, which
copies the constructor and both dispatch bodies verbatim and asserts no dispatch occurs.

**Still outstanding for Stage 1: it has not been compiled for ARM or heard on hardware.**
Neither is possible in a sandbox — the toolchain download is proxy-blocked and the host
compiler is too old for this codebase's C++23. Build and listen before building Stage 2 on it.

### Restaged, cheapest-first

The original plan put the echo first and everything audible behind three stages of plumbing.
Given the page decision in §6, that is the wrong order: it leaves the page empty until Stage 4
and defers the first hardware listen until after the hardest part is already built on
unverified foundations. Velocity and Gate Time cost far less and prove the seam on hardware
early. Reordered accordingly.

**Stage 2 — `MidiFx` interface, minus the pool.** The interface from §4 and the settings
object, but *not* §3's note pool — nothing yet needs a note to outlive its parent. `flush()`
wired to transport stop and clip stop from the start, because the paths are easier to enumerate
while the feature is small. Ship with a null effect. Success: nothing audible changes.

**Stage 3 — Velocity, with its page. DONE**, `8d82876f`. §5a plus the page scaffolding.
**First listening test, and it should be a boring one.**

Writing a real effect exposed a defect in Stage 2's seam placement, which is the argument for
having built a trivial effect first. Stage 2 ran the note-event axis from `sendNote` only —
which covers every note with the arp **off**, but an arpeggiating note is emitted from
`render()` and `doTickForwardForArp()` instead. An effect wired that way works with the arp off
and silently does nothing with it on: precisely the failure this whole feature exists to
prevent. **The invariant now holds everywhere and is stated on `MidiFxChain`: the note-event
axis runs immediately before every dispatch, on every path**, and after the time axis so
note-mapping effects also see whatever a time-based effect just generated.

That placement forces **idempotence**, which every future effect inherits. With the arp off, a
Sound that cannot get a voice leaves its note `PENDING`, and `handlePendingNotes` hands the same
`ArpNote` element back on a later render — so the seam can run twice on one note. `VelocityFx`
computes from `baseVelocity`, which the arp writes once per note-on and never touches again,
rather than offsetting the running value. **Any effect that mutates an `ArpNote` in place must
do the same.**

**Stage 4 — Gate Time. DONE**, `edbdb810`. §5b, second page on the group, first use of the
tick axis.

**§5b was right that it needs the clock and wrong about the swallow.** The design assumed the
early note-off had to suppress the sequencer's real one at the note's written end. It does not:
a second note-off for an already-released note is harmless on MIDI, CV and a Sound voice alike,
while suppressing it would mean matching note-offs to notes across retriggers — a good way to
swallow the wrong one and hang a note. Not swallowing removed most of the stage. Worth
re-checking under MPE, where a stale note-off could touch a member channel that has since been
reassigned.

`MidiFxContext` gained `arpIsOn`, because the arp has its own Gate and two gates fighting over
one note is not a feature. Effects generally should not care whether the arp is running — that
is the point of the seam — so **be suspicious if a third effect wants that field.**

Pending note-offs are stored **by value**. `arpeggiator.notes` is a `ResizeableArray` that moves
its elements on delete, so a pointer into it dangles silently the moment any *other* note is
released. This is the same lifetime problem §3 describes, met early and dodged rather than
solved: Gate Time only ever shortens, so its note-off always lands while the parent is alive,
and it needs identity but not a pool.

**Stage 5 — the note pool.** §3 option B, plus the MPE survey extension in
`midi_instrument.cpp:846-866`. Separated from the echo on purpose: the pool is the subtle part
and deserves its own review and its own hardware pass, not to arrive tangled up with an
effect's behaviour.

**Stage 6 — note delay / echo.** §5c. Hardcoded params first — fixed division, 3 repeats,
75% feedback — for the listening test, then its page. Test on MIDI out first (easiest to verify
against a scope or a DAW), then CV, then a synth.

**Stage 7 — 1.2.1 backport.** See §8. Note the reorder helps here too: Velocity backports
almost for free and could go early if the release line wants something to show.

**Stage 8 — hardware pass + upstream.** Format-check first (`dbt format`); per HANDOFF
nothing on this line has ever been format-checked.

Stages 1–3 are the natural upstream set: the seam commits, the decoupling, and one small
complete feature that demonstrates why the decoupling was needed.

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
5. **Should MIDI/CV call `handlePendingNotes` when the arp is off, as `Sound` does?** Found
   while building Stage 1 and left alone there. Today, switching the arp off while holding a
   key restarts the note on a synth and silences it on MIDI/CV. Consistency argues yes; it is
   a behaviour change either way and wants a deliberate decision plus a hardware listen, not a
   drive-by fix inside a larger stage.
6. ~~**What is the RS7000 target, exactly?**~~ **Settled.** The target is a Play FX page, not
   a single note delay. MIDI FX gets its own page group sibling to the Arpeggiator, one page
   per effect, and the stages are reordered cheapest-first so the page has content early.
   §5, §6 and §7 rewritten accordingly. Clock Shift and Beat Stretch are explicitly out —
   they act on the sequenced phrase, not on note events.
7. **Automatable params, or plain settings?** §6 flags the cost: every `UnpatchedShared` param
   is an `AutoParam` on every `ParamManager`, whether or not the clip uses MIDI FX. Automatable
   is more useful and more expensive. Measure before committing to a param count.
8. **Should MIDI FX velocity stack with arp velocity spread, or override it?** Currently
   overrides, as a consequence of the idempotence anchor (§5a). Stacking is possible but needs
   somewhere to keep the pre-FX value, which is the same problem the note pool solves — worth
   revisiting at Stage 5 rather than before.
9. **Does Gate Time want a live-note mode?** v1 is percentage-for-sequenced,
   pass-through-for-live (§5b). A fixed absolute gate for live notes is defensible but is
   really a second control with a second name, not a mode of the first.

---

## Appendix — key references

| What | Where (1.3, `e3402639`) |
|---|---|
| Dispatch seams | `non_audio_instrument.cpp:153,168`; `sound.cpp:1596` |
| Arp pass-through when OFF | `arpeggiator.cpp:375-383` (on), `:397` (off) |
| Arp-independent tick precedent | `handlePendingNotes`, `arpeggiator.cpp:486` |
| The three gates (only the first is real — see §1 correction) | `non_audio_instrument.cpp:37`; `arpeggiator.cpp:1484,1560` |
| Stage 1 seam | `modulation/midi_fx/midi_fx.h` on `feat/midi-fx-stage1-13` |
| Chain, settings, effects | `modulation/midi_fx/midi_fx_chain.h`, `midi_fx_settings.h`, `velocity_fx.h` |
| The page strip itself | `horizontalMenusChainForSound` / `...ForMidiOrCv`, `menus.cpp:1975-2012` |
| MIDI FX menu items | `gui/menu_item/midi_fx/` |
| Sequenced note length, in ticks despite the name | `note_row.cpp:2558` → `sampleSyncLengthOn` → `voice.cpp:251` |
| Page machinery | `gui/menu_item/horizontal_menu.h`, `horizontal_menu_group.h` |
| The arp's pages, as the worked example | `gui/ui/menus.cpp:363-404` |
| Sound editor roots needing the new entry | `menus.cpp:1477,1510,1655,1672,1680` |
| Sequenced note length reaching the seam | `note_row.cpp:2558` (`sampleSyncLength`) |
| Tick scheduler | `session.cpp:2522`; `arrangement.cpp:311` |
| Sound's inline voice-gated path | `sound.cpp:1544-1566` |
| Sound's other note-on entry | `process_postarp_notes`, `sound.cpp:2367` |
| `ArpNote` / `ArpReturnInstruction` | `arpeggiator.h` (`ARP_MAX_INSTRUCTION_NOTES = 4`, line 37) |
| MPE member channel survey | `midi_instrument.cpp:846-866` |
| Param enum + by-name serialization | `param.h:206-224`; `param.cpp:425,783` |
| Arp settings XML | `arpeggiator.cpp:1847`; `instrument_clip.cpp:2380,2653` |


---

# Postscript: what hardware said

Written 2026-09-10, after the whole set was built, flashed and played. Everything above this line is
the design as it was reasoned out; this is where it met a Deluge.

## The seam held

Stages 1–4's central bet — that the note-event seam is the right place for note effects, and that
the arp-independent tick path already existed — survived without amendment. Velocity, Gate Time and
the Delay sit exactly where §4 said they would, and the first ARM build of roughly 1,300 lines
compiled clean and ran.

## Three things the design got wrong

**A percentage is not a control.** §5b treats Gate Time as a percentage and stops there. On hardware
that is 375 knob positions whose useful ones are bunched at both ends, and worse, the readout lies
about its own resolution: ticks are integers, so 101% of a sixteenth note is the *same number of
ticks* as 100%. Nothing happened for the first several positions and nothing "opened up" until past
200%. Both controls that are ratios — Gate Time and Beat Stretch — are now steps of 2^(1/12), zero
neutral, ±12 to halve or double, ±24 for a quarter or four times, with the resulting percentage
still on screen. See `gui/menu_item/midi_fx/ratio_step.h`.

**Taking a note over is not free.** Under the same fault the note at 101% was not longer, it was
*quieter*: the gate had not changed, but this effect had taken responsibility for the release from
the sequencer, and the release landed on the same tick as the next note. An extension shorter than a
couple of ticks is now no extension at all, and a pending release is dropped when the same note code
starts again — the two notes are indistinguishable at the output, so releasing "the old one"
releases the new one.

**Kits are not one thing.** §2 recommends Kits out of scope and §6's correction says the pages hide
there. Both were overtaken: the effects are per drum. Play FX started out clip-wide, which is what
the RS7000 does — Play FX is per track and a kit is one track — and on hardware that reads as a
fault: selecting one drum and shifting it moved the whole kit, a muted row included. A row is a drum
on a Kit and a pitch on a melodic clip, so the two now split on exactly that, with AFFECT ENTIRE as
the kit-wide gesture.

## The menu is a system, not a list

The AFFECT ENTIRE work took three commits and only the last one mattered, which is worth recording
because the first two were reasonable and useless.

The pages hid themselves under AFFECT ENTIRE because `SoundEditor::setup` only points
`currentMidiFxSettings` at a drum's settings when a drum is selected. Making every item resolve its
own target fixed that (`2c96b7f9`). Making AFFECT ENTIRE win over however the menu was opened fixed
a second, real hole (`1bc2cf28`). Neither helped, because `soundEditorRootMenuKitGlobalFX` — the root
the editor actually opens in that mode — never contained `&midiFxMenu` (`8e2867c9`).

§6 already warned about this class of mistake, in the sentence "there are more of these than is
obvious, and missing one is how a feature ends up working on synths but not on MIDI". The warning
was right and still not enough: the count in that table was corrected *downward* during Stage 3, and
the root that turned out to be missing is not in the table at all.

## Play FX and the two frames

Clock Shift and Beat Stretch have their own design note, `docs/PLAY-FX-SEQUENCER-DESIGN.md`. The one
thing worth repeating here: the transform belongs to the playback read and nowhere else.
`NoteRow::processCurrentPos` finds notes in the shifted frame and, when one should already have
started, hands it to `attemptLateStartOfNextNoteToPlay` — which measured how late it was against the
*raw* position. Two frames, one note, feeding a sample offset. It behaved until enough was happening
at once, which is the signature of every bug of this shape.

## Open

- **One freeze, unexplained.** A `freezeWithError` on hardware with several effects running. The code
  was lost to glare on the photo and it has not recurred since, across a synth and a kit together
  with Play FX engaged. Recorded rather than closed.
- **Resume paths still read the raw position.** `resumePlayback` and the other `getLivePos` readers
  are not in the Play FX frame. Reached when playback resumes rather than every tick.
- **The host tests are behind.** They are not in the firmware build; `tests/stage3/README.md` and
  `tests/stage4/README.md` say what changed under them.
- **Not format-checked.** Nothing on this line has been through `dbt format`.
- **Open question 2 from §10 is still open.** Whether an echo should feed the record path was never
  decided, because nothing has recorded through one yet.
