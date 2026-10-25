# Rudement features

The additions in this fork that are not part of official or upstream community firmware, and the
Community Features toggles that control them.

> **Withheld from the 1.3 release:** AUX Sends, AUX Select, Hold Loop Pad / Loop (hold) and Track Default.
> Their code is still in the tree, but the switches and menus are hidden and they are forced Off. The
> sections below that describe them are kept for reference.

This file is deliberately fork-local. The upstream page at
`website/src/content/docs/features/community_features.mdx` is left alone: it is shared with
SynthstromAudible, it changes on nearly every merge, and anything written there would ride along in a
PR back upstream.

Every feature below can be switched off from `SETTINGS > COMMUNITY FEATURES > RUDEMENT UPGRADES`. **Doing so hides the
feature's menus but leaves its processing in place**, so a song already using one still plays back
exactly as saved. This is deliberate, and is how the stock `Enable DX7 Engine` toggle already behaves.

| Toggle | 7SEG | Default | Hides |
|---|---|---|---|
| Four-Band EQ | `EQ4` | On | The two mid EQ bands |
| Plaits Engine | `PLTS` | On | `PLAITS` in the oscillator type list |
| Clouds FX | `CLDS` | On | The Clouds menu |
| Gristleizer | `GRIS` | On | The Gristleizer menu |
| Sear | `SEAR` | On | The Sear drive and tone controls |
| Kit Split | `SPLT` | **Off** | `Kit Global FX > Actions > Split Kit` |
| AUX Sends | `AUX` | On | The `AUX` menus and `SETTINGS > AUX` |
| Temp Hold | `HOLD` | **Off** | No menu of its own — off means SHIFT stops holding values |
| Harmonize | `CHRD` | **Off** | The `Harmonize` page in MIDI FX, and the re-pitching itself |
| Strum | `STRM` | **Off** | The `Strum` page in MIDI FX, and the strumming itself |
| Hold Loop Pad | `HLDP` | **Off** | No menu of its own — off means the grid's red loop pad toggles as stock |

**Most of MIDI FX has no toggle**, on the grounds that it needs none: every effect in it is off per
clip until switched on, and a song saved before it existed loads with the whole page group inert. So
Velocity, Gate, Repeat, Delay, Spice, Play FX, Generate, Spread and Stochastic are simply always
there. Harmonize and Strum are the two exceptions, and only because each is a whole new behaviour
rather than a variation on an existing one — each is still off per clip on top of its toggle, so
neither can change an existing song. Giving the rest toggles too is a small job if you would rather
it matched.

---

## Four-Band EQ

The stock EQ has two bands, bass and treble. Two bell bands are added between them, and every EQ
control now reports what it is actually doing rather than a bare `0-50`:

- `Low mid` / `LMfq` — bell band, gain and centre frequency.
- `High mid` / `HMfq` — bell band, gain and centre frequency.
- Amount controls read out in dB, frequency controls in Hz.

The three readouts are derived differently, and each for a reason. Bass is exact and needs nothing but
the amount, because a lowpass is exactly 1 at DC. Treble reads its own `Freq` param, because a digital
lowpass is nowhere near 0 at Nyquist, and because above a `Freq` knob value of 32 the treble amount does
nothing at all. The two bells take a fixed capture at their neutral centre rather than tracking `Freq`,
to stay monotonic through the region where the amount law over-cuts into polarity inversion.

Switching the toggle off hides the two mid bands only. Bass and treble are stock Deluge and stay where
they are, as does the dB and Hz reporting on them.

Source: `src/deluge/gui/menu_item/eq/`, `src/deluge/dsp/eq_bands.hpp`

## Plaits

Mutable Instruments Plaits, as an oscillator type with its 24 synthesis models. Select `PLAITS` as the
oscillator type, then press select on it again to enter the Plaits menu:

- `Plaits model` — which of the 24 engines is running.
- `Harmonics`, `Timbre`, `Morph` — the module's three main controls, in its own front-panel order.
- `Low-pass gate`, `LPG decay`, `LPG colour` — what the gate does to the engine.
- `Aux output` — which of the engine's two outputs you take.

Plaits is available on oscillator 1 only and not in kits, the same restriction DX7 has.

Switching the toggle off removes `PLAITS` from the oscillator type list. A sound already set to Plaits
keeps playing as Plaits — the type simply has no row to sit on in the list until the feature is switched
back on, and the menu shows the first entry rather than an out-of-range index.

Source: `src/deluge/dsp/plaits/`, `src/deluge/gui/menu_item/osc/type.h`

## Clouds

Mutable Instruments Clouds, from the Parasites alternate firmware rather than stock, as a global effect.
It appears in the horizontal menu chain for sounds, kits, songs and audio clips.

- `Clouds Mode` — `Granular`, `Stretch`, `Delay`, `Spectral`, `Oliverb` or `Resonestor`. Set to `OFF`,
  the engine is not running and the rest of the menu does nothing.
- `Freeze` — latched rather than momentary, because the Deluge has no spare panel button for it and a
  latched freeze is the more useful of the two live. Only offered while the engine is on: freezing a
  buffer that is not being recorded into does nothing audible.
- Nine parameters — `Position`, `Size`, `Pitch`, `Density`, `Texture`, `Blend`, `Spread`, `Feedback`
  and `Reverb`.

Nine parameters plus a mode and a freeze is why Clouds is its own submenu rather than another entry in
the mod-FX type list: mod FX has four param slots and Clouds needs more than twice that.

Source: `src/deluge/dsp/clouds/`, `src/deluge/gui/menu_item/clouds/mode.h`

## Gristleizer

A modulation and distortion effect with a master switch, so it can be bypassed without unwinding its
settings.

- `On` — master switch.
- `Rate`, `Depth`, `Shape`, `Bias`, `Mode` — the modulation.
- `Frequency`, `Resonance` — the filter.
- `Dirt`, `Level` — drive and output trim.

## Sear

Drive, previously called Heat. Two controls:

- `Sear` — patched, so it is per-voice and can be modulated like any other patched parameter.
- `STon` — unpatched tilt tone control, shaping what the drive bites on.

## Kit Split

Explodes a kit clip into one single-sound kit, and one clip, per row that has notes. On a kit clip with
affect-entire on: `Kit Global FX > Actions > Split Kit`. Stepping into the submenu is the confirmation,
and its label states how many kits you are about to get.

Every row that has notes becomes its own kit holding exactly that one sound, with its own clip carrying
only that row's notes. In grid mode they are N new columns, drum 0 leftmost. The source clip and kit are
consumed.

| | |
|---|---|
| Notes, velocity, probability, iterance, fill | travel |
| Per-row FX — filters, envelopes, patch cables, sends, sidechain | travel |
| Kit-global volume and pan | travel |
| Other kit-global FX — delay, reverb, filter, mod FX | **do not** — new kits come up neutral |
| Kit arpeggiator | does not — it indexed drums that no longer share a kit |
| Rows with no notes | skipped; those drums go away with the source kit |

Volume and pan travel because they are the output's gain and position rather than wet effects, and each
new kit holds exactly one drum: copying them across reproduces the mix instead of multiplying it.
Inheriting the delay would have turned one delay into N.

The new clips come up in affect-entire, like the source, with their one drum selected.

It refuses to run, and says so, if playback is running, the clip has fewer than two rows with notes, the
clip is the sync-scaling clip, the clip has instances in the arranger, or there are more than 64 rows
with notes.

**Not undoable.** The undo history is cleared before anything moves, exactly as creating a new
instrument does. Allocation is all done up front, so if RAM runs out the song is untouched and you get
an error rather than half a split. See `KIT-MERGE-DESIGN.md` for what making it undoable would take.

Pressing `MIDI` or `CV` on a split track says *Clip not empty* (`CANT` on 7SEG). That is stock
behaviour, not something the split introduced — any kit clip with notes does the same.

Unlike the other features here, Kit Split is off by default, because it is destructive.

Source: `src/deluge/model/instrument/kit_split.cpp`, `src/deluge/gui/menu_item/kit/split.h`

---

## AUX Sends

An assignable audio send bus on the CV output sockets, ported from sticknobills'
`AUX-Sends-1.0.1`. The two CV sockets carry audio instead of pitch voltage, so a track can be sent
out to an external mixer, pedal or recorder while still going to the main outs.

A new `AUX` submenu appears on the Kit-global, Audio Clip and Sound roots:

- `MAIN` — toggle. Whether this Clip still reaches the main outputs.
- `CV Send`, when `SETTINGS > AUX > Split` is off — one send, because the pair is one stereo
  destination and placement across it comes from the Clip's own pan.
- `CV1 Send` / `CV2 Send`, when Split is on — two sends, because the pair is then two independent
  mono aux buses, which is what every mixer does.

These are real params: automatable, LEARN-able, assignable to a gold knob, and present in the
shortcut grids.

`SETTINGS > AUX` holds what belongs to the rig rather than to a clip — `CV1 Level`, `CV2 Level` and
`Stereo Split`. It is deliberately not inside `SETTINGS > CV`, which is about pitch voltages.

On the Sound root the menu hides itself when that root is doubling as the Drum editor: every Drum
carries its own copy of the shared send params, but the capture can only isolate a whole track, so
those per-Drum copies could never do anything. The kit-wide send in `KIT GLOBAL FX` is the one that
works. The firmware already does this for portamento.

7-segment only, matching the original release — no OLED support was added in the port.

Source: `src/deluge/processing/engines/cv_audio_stream.cpp`,
`src/deluge/gui/menu_item/cv_output/routing.h`

---

## MIDI FX

Note effects that run whether or not the arpeggiator is on, modelled on the Yamaha RS7000's Play FX.
Its own page group, sibling to the Arpeggiator rather than buried inside it, because the whole premise
is that it is arp-independent.

`MIDI FX` / `MFX` appears on Synth, MIDI and CV clips, on Kit drums, and on the Kit-global root under
AFFECT ENTIRE. Everything is **per clip**, saved with the song, and **off by default** — a song from
before this existed loads with all of it inert.

On a Kit the settings are **per drum**: select a drum and you edit that drum, hold AFFECT ENTIRE and
you edit every drum at once. This is the rule for the whole page group, however you reached the menu.

| Page | 7SEG | Controls |
|---|---|---|
| `Enabled` | `MFX` | Master switch for the clip (or the drum). Off by default. |
| `Velocity` | `VELO` | `Offset` / `OFST` |
| `Gate` | `GATE` | `Gate` / `GATE` |
| `Repeat` | `RPT` | `Repeats` / `RPTS`, `Rate`, `Decay` / `DECY` |
| `Delay` | `DLAY` | `Rate`, `Repeats` / `RPTS`, `Feedback` / `FDBK`, `Pitch` / `PTCH`, `Reset` |
| `Spice` | `SPCE` | `Spice` / `SPCE`, `Dice` / `ROLL`, `Reset` |
| `Play FX` | `PFX` | `Shift` / `SHFT`, `Stretch` / `STCH`, `Reset` |

### Velocity

`Offset`, −64 to +63, added to the note's velocity and clamped to 1–127. Never 0: velocity 0 is
note-off on MIDI, and emitting it would be a hanging note wearing a disguise.

With the arp on, a non-zero offset **overrides** the arp's velocity spread rather than stacking with
it. The effect anchors on the note's base velocity so that running twice over the same note cannot
compound, and the spread lives in the gap that anchoring discards. At zero it writes nothing, so the
spread is untouched whenever this is idle.

### Gate

A ratio on a **sequenced** note's written length, ×0.25 to ×4, shown as a percentage.

The control is steps of a semitone's worth of ratio rather than raw percent: zero is neutral, ±12
halves or doubles, ±24 is a quarter or four times. Percent alone was 375 knob positions whose useful
ones bunched at both ends, and it lied about its own resolution — ticks are integers, so 101% of a
sixteenth note is the same number of ticks as 100%.

- **A held key passes through untouched.** It has no length until it is released, and a percentage of
  an unknown number is not a thing.
- **It switches itself off while the arpeggiator runs.** The arp has its own Gate, and two gates
  fighting over one note is not a feature.
- **Under 100%** the note is released early and the sequencer's own note-off is let through as normal.
- **Over 100%** the note is held past its written end, which means the sequencer's note-off has to be
  suppressed and this effect owes the note a release of its own. Every way out of that debt is
  covered: the timer, a loop, a retrigger of the same note, and transport stop. An extension shorter
  than a couple of ticks is treated as no extension, because taking the note over for nothing is how
  short notes end up quieter rather than longer.

### Repeat

A drum hit repeating itself, Elektron retrig territory and the reason MIDI FX exists on Kits at all.

`Repeats` 0–16, `Rate` as a musical division (triplet and dotted come free), `Decay` as a velocity
multiplier per hit, 0–100%.

**Free-running, not synced** — it runs on the audio clock, so hitting a pad with the transport
stopped still rolls. Four simultaneous rolls; a fifth note plays undelayed rather than stealing one,
because a half-emitted roll sounds like a fault.

### Delay

The RS7000's MIDI Delay. `Rate` as a musical division, `Repeats` 0–16, `Feedback` as a velocity
multiplier per repeat, `Pitch` −24 to +24 semitones added per repeat.

**Synced, so with the transport stopped it does not sound.** Repeat is the free-running one. A repeat
that fades below velocity 1, or transposes off the end of the keyboard, ends the tail there rather
than clamping — a clamped tail repeats one pitch forever and reads as broken.

Releasing the key does not kill the tail; that is the entire point, and it is why the effect owns its
own note records rather than borrowing the arpeggiator's, which are deleted the moment a key comes up.
The tail rings across the clip's loop point. Transport stop and clip stop release everything it is
holding, including on MIDI and CV outputs, where nothing else in the firmware knows those notes exist.

### Spice & Dice

Arturia's MicroFreak pair, on a Deluge clip. `Spice` is how far the gates wander from what you
wrote; `Dice` is which variation you are hearing.

- `Spice`, 0–50. At 0 the clip plays exactly as written. Turned up, steps start dropping out and
  surviving notes get shorter gates — at 50, a little over half the steps are gone.
- `Dice`, 0–99. **Turning this knob is the roll.** Each number is a variation, and the same number
  is always the same variation, so the bar you liked two turns ago is two turns back rather than
  gone.

Both save with the song, so a groove you rolled into comes back exactly as you left it.

**Nothing is written into the clip.** Both halves are read at playback time, so the notes, their
lengths and their probabilities are untouched — turn Spice back to 0 and the original sequence is
exactly where you left it. There is nothing to undo and nothing recording can bake in: record a
spiced clip into the arranger and you get what you wrote, not what you heard.

The variation is a hash of (Dice, row, step), not a stream of random numbers. That is the whole
difference between this and the Deluge's own per-note probability: probability rolls again every
bar, and Spice repeats until you roll it. It survives the loop, a clip relaunch, and a stop and
start.

On a Kit it is **per drum**, like the rest of MIDI FX — hold AFFECT ENTIRE to spice the whole kit
at once. Each row still makes its own decisions, so the kit thins out rather than stuttering as one
block.

**Rolling without looking.** `SETTINGS > MIDI > COMMANDS > Dice` learns a note, CC or program
change, so a footswitch or a pad rolls a new variation mid-performance. It is song-wide — a
footswitch has no way to say which track it meant, so one press moves everything that is spiced to
the same new variation. Tracks at Spice 0 are left alone, and each row still decides for itself, so
the song changes together without the whole thing moving in lockstep. It is momentary: there is
nothing to hold, and nothing happens on the note-off.

**Order of operations.** Spice runs last and only ever subtracts. A note that failed its own
probability, iteration or fill condition stays failed — the dice never resurrects anything you
explicitly wrote a condition for — and `FILL` notes are exempt from being dropped, because Fill is
already a deliberate performance gesture.

Spiced notes go out MIDI and CV as well as to the internal sound, because they all share one
dispatch path. On a MIDI clip that means Spice thins the pattern reaching your external gear, which
is the point rather than a side effect.

#### Known limits

- **Gates only shorten, never lengthen.** Holding a note past its written end means suppressing the
  sequencer's own note-off and owing the note a release — the debt Gate Time takes on above 100%.
  Use Gate Time for that.
- **Silent steps stay silent.** Arturia's Spice also fires steps that have no note. That needs the
  sequencer to wake on a grid rather than on note events, and to own note-offs for notes the clip
  does not contain. It is phase 2, and it is where the CPU risk is — see
  `SPICE-DICE-FEASIBILITY.md` §2.
- **Drones are left alone.** A note that fills its whole row takes the constant-note path, which
  deliberately does not stop the sound.
- No gold knob, no automation lane, no CC on the amount. That is the price of it being a MIDI FX
  setting rather than a parameter — and what it buys is MIDI and CV clips, which carry no parameter
  set of their own.

### Play FX

The two RS7000 effects that act on the phrase rather than on note events. They move where the
sequencer reads, and touch no notes — nothing is baked in, and recording still records what you
played.

- `Shift` — the phrase plays early or late, ±192 ticks, a bar either way at the default resolution.
- `Stretch` — the phrase plays at a different rate against the clip's length, ×0.25 to ×4, on the same
  stepped control as Gate. Under 100% it advances faster and repeats inside the loop; over 100% it
  advances slower and its tail is truncated at the loop point, which is what the RS7000 does.

Automation deliberately keeps running at the clip's own rate rather than stretching with the notes.

On a Kit these are per drum, which is the point — pushing the hats late against the kick is a thing
people want, and shifting the whole kit is not. Hold AFFECT ENTIRE for the whole kit.

### Reset

Every page ends with `Reset`, and the MIDI FX submenu has `Reset all` / `RALL`.

It reads as well as writes: the value shows `Default` / `DFLT` when that effect is untouched and
`Edited` / `EDIT` when it is not, so a pass across the pages says which effects are actually doing
something. Turning it to `Default` performs the reset; turning it to `Edited` does nothing and springs
back, because un-reset is not something it can honestly offer.

It leaves the master switch alone — that is not a setting of any one effect — and it does not cut a
tail that is already ringing, which fades out as it would if you turned Repeats to zero by hand. On a
Kit, Play FX's Reset clears every drum, because a control whose job is getting you back cannot read
`Default` while three other drums are still shifted.

### Known limits

- No free-running Delay. It is a second timing implementation; Repeat is the free-running effect.
- Gate does nothing to live notes, in either direction.
- No shortcut pad. The pages are reached by navigating in.
- Whether a repeat should feed the record path is undecided, because nothing has recorded through one
  yet.

Source: `src/deluge/modulation/midi_fx/`, `src/deluge/gui/menu_item/midi_fx/`,
`src/deluge/model/note/note_row.cpp` (Play FX, and Spice's gate half),
`src/deluge/model/clip/instrument_clip.cpp` (Spice's drop half). Design notes in
`docs/MIDI-FX-DESIGN.md`, `docs/PLAY-FX-SEQUENCER-DESIGN.md` and `SPICE-DICE-FEASIBILITY.md`.

---

## Temp Hold

Hold **SHIFT**, change anything, and everything you changed goes back to where it was when you let
go. It adds no control of its own. It changes what every control already on the instrument is *for*,
by making "commit to this" and "try this" two different gestures.

Off by default, and deliberately: SHIFT is held constantly for other reasons, and turning every held
SHIFT into a hold that reverts would change the instrument under someone who never asked for it.

**A footswitch.** `SETTINGS > MIDI > COMMANDS > Temp Hold` learns whatever you send it, and answers
on both halves — press engages the hold, release puts everything back. This is the form that matters
live, because it is the only one that leaves both hands on the controls.

`COMMANDS > Shift` reaches the same feature, since it drives the button Temp Hold watches — but it
engages the whole SHIFT modifier, so every pad and button does its shifted job while your foot is
down. The dedicated command engages the hold and nothing else.

**Either half holds, and the hold ends when both are clear.** Releasing SHIFT with a foot still on
the switch leaves everything where it is, and the reverse likewise. Without that the two halves fight
and whichever is let go of first wins.

**Sticky shift gives a latched Temp for free.** With `SETTINGS > COMMUNITY FEATURES > Shift Is
Sticky` on, a short press arms the hold, you change what you like, and a second short press puts it
all back.

### What it reaches

| | Covered |
|---|---|
| Gold knobs | Any param the knobs currently address, in any view — clip, song, arranger, automation |
| MIDI FX | Velocity, Gate, Repeat, Delay, and `Enabled` |
| Spice & Dice | Both |
| Play FX | Clock Shift and Beat Stretch |

Song mode needed no work of its own. The view does not own the param, it owns a pointer to whatever
the knobs currently address: in a clip that is the clip, under AFFECT ENTIRE it is the song. Same
snapshot, same restore, a different `ModControllable`.

On a Kit, MIDI FX and Play FX follow the rule the rest of that page group follows — one drum, or
every drum under AFFECT ENTIRE. A hold taken with AFFECT ENTIRE down restores the whole kit.

### What it does not reach

- **The sound editor's menu-writing path.** Bouls, Wisps, Gristleator, Sear and the EQ are covered
  only where their params sit under a gold knob, not when you edit them in a menu.
- **Actions.** Kit Split, Scene Capture and Grid Column Reorder do things rather than hold values, so
  there is nothing to put back.
- **Tempo and transport.** The T-1's Temp does not cover them either.

### Automation is switched off while it is held

A param write during a hold takes the route the stutter param already takes: the timeline counter is
nulled on a copied model stack, so the write is a live value change that cannot land in an automation
lane. Without it a temp twist would carve a real curve into the clip and the restore would carve a
second one over the top.

**So you cannot record automation while SHIFT is held.** That is the cost of the guarantee, and it is
the right trade — the whole point is that nothing survives the release.

### Known limits

- Menu values outside MIDI FX are not held (above). Reaching them needs a second hook on the sound
  editor's write path.
- A hold is capped at 16 distinct params and 48 settings objects. Past that the extra changes are let
  through **unheld** rather than dropped — a knob that does not come back is a smaller surprise than a
  knob that did nothing when you turned it — and `Too many changes to undo` / `FULL` appears once.
- A hold that spans a song load restores nothing. Everything it was holding belongs to a project that
  is no longer loaded.

Source: `src/deluge/modulation/temp_hold.{h,cpp}`, with the three hooks in
`src/deluge/gui/views/view.cpp` (`modEncoderAction_existentParam`),
`src/deluge/gui/menu_item/midi_fx/target.h` (`midiFxApply`) and
`src/deluge/gui/menu_item/midi_fx/play_fx.h` (`playFxWriteRows`); the release fires from
`src/deluge/hid/buttons.cpp`. The MIDI command is `GlobalMIDICommand::TEMP_HOLD`, dispatched
both ways in `src/deluge/playback/playback_handler.cpp` and persisted in flash bytes 205-210.
Design notes in `TEMP-HOLD-FEASIBILITY.md`.

---

## Track Edit

Change a parameter once and every clip on that track follows. It is a temporary mode in **song
view** that points the gold knobs at a whole track instead of one clip. Nothing about the mode is
saved with the song; only the parameter values it wrote are.

It has no toggle and no menu. It cannot change an existing song, because it does nothing until you
turn it on.

**Turn it on.** In song view, hold **SHIFT** and press **AFFECT ENTIRE**. The AFFECT ENTIRE LED
flashes and, on an OLED, the song name in the title area is replaced by `PICK A TRACK`.

**Pick a track.** Press any pad belonging to the track. The whole track
flashes white and the title changes to `TRACK EDIT`.

**Turn the gold knobs.** Whatever you change is written to every clip on that track — session clips
and arrangement-only clips alike.

**Open the track's menu.** Press **SELECT**. Instead of Song FX you get the picked track's sound menu, and
anything you change there is written to every clip on the track, the same as a knob turn. This is how the
four EQ bands, and every other parameter that has no gold knob, reach the whole track. Press **BACK** to
come out of the menu; you are still in Track Edit, with the same track picked. With no track picked yet,
SELECT says `PICK A TRACK`.

**Turn it off.** Press SHIFT + AFFECT ENTIRE again, or leave song view. AFFECT ENTIRE goes back to
exactly what it was before, and the song name returns.

### Kits

A Kit has two targets, and the title says which one is live:

| Title | Knobs reach |
|---|---|
| `WHOLE KIT` | The kit's own parameters, in every clip using that kit |
| `DRUM ROW` | The selected drum's parameters, in every clip using that kit |

Press **AFFECT ENTIRE** while the mode is on to switch between them. SELECT opens the menu for whichever one is
live: the kit's own menu for `WHOLE KIT`, the drum's sound menu for `DRUM ROW`. `NO DRUM` means there is no
drum selected yet. A kit remembers its selected drum only from clip view, so open the kit clip, tap a
drum row, and come back. On a 7-segment display, where there is no title, `WHOLE KIT` / `DRUM ROW`
appears as a popup at the start of each knob turn.

### What it reaches

Patched and unpatched sound and global parameters: level, pan, filters, envelopes, delay, reverb,
and the rest of what the gold knobs address.

### What it does not reach

- **Modulation depths**, which are indexed by cable and can differ from clip to clip.
- **MIDI and expression** parameters.
- **Clip settings that are not parameters**, such as the arpeggiator, MIDI FX, and Spice & Dice. They
  appear in the menu but change only the clip the menu opened on.
- **MIDI and CV tracks** have no sound menu to open; SELECT says `NO MENU`.
- **Morph**, which is a performance gesture with state of its own rather than a mix setting.
- **Automation curves.** A knob turn writes a value to each clip. It does not copy one clip's
  automation onto the others.

### Known limits

- The mode lives in song view only. Leaving song view ends it. Leaving the menu with SONG rather than
  BACK also ends it.
- Clips added to the track later are not touched until you turn a knob again.
- Every knob click rewrites every clip on the track, so a very large track costs a little CPU while
  you turn.

Source: `src/deluge/model/song/track_link.{h,cpp}`, with the funnel hook at the end of
`AutoParam::setCurrentValueInResponseToUserInput` in `src/deluge/modulation/automation/auto_param.cpp`,
the pad, AFFECT ENTIRE and SELECT handling in `src/deluge/gui/views/session_view.cpp`, the menu's clip
context in `src/deluge/gui/ui/sound_editor.cpp` (`editingAClip`), and the knob targeting in
`src/deluge/model/clip/instrument_clip.cpp` (`getActiveModControllable`). Design notes in
`TRACK-LEVEL-PARAMS-FEASIBILITY.md`.

---

## Hold to Loop

Hold a footswitch to record a loop; let go and it plays. It is the stock LOOP command with a release added,
learned to its own switch, so the stock LOOP command keeps working exactly as before.

The footswitch command has no toggle: it does nothing until you learn a switch to it. The pad version below has one.

**Set it up.** SETTINGS → MIDI → COMMANDS → **Loop (hold)**, then learn it to a footswitch or any MIDI note
or CC button.

**Use it.**
- **First loop, Deluge stopped.** Hold the switch: playback starts and the armed clip records. Let go: the loop
  closes and plays. With nothing else in the song, the loop is exactly as long as the hold, and it sets the tempo.
- **Overdubs while playing.** Hold the switch: an overdub is set to start at the top of the next loop. Let go:
  it records to the end of that pass, then plays. Let go before the overdub has started and nothing is recorded.

Letting go only ever ends or cancels the loop that the same press started. If the loop already stopped some other
way, letting go does nothing; it never starts a new recording.

Pressing it while a loop is already recording is the stock second press: it ends that loop.

**Without a footswitch: the grid's red loop pad.** Turn on SETTINGS → COMMUNITY FEATURES → Rudement upgrades →
**Hold Loop Pad**, and have the stock *Grid View Loop Pads* setting on, so the loop pads show in grid view's
right-hand column. The red loop pad then works the same way: hold to record, let go and it plays. With Hold
Loop Pad off, the red pad is the stock press-to-toggle LOOP. The magenta layering pad is not changed. Grid view only:
the rows layout has no loop pad.

Source: `PlaybackHandler::loopHoldPress` / `loopHoldRelease` in `src/deluge/playback/playback_handler.cpp`.
Design notes in `HOLD-TO-LOOP-FEASIBILITY.md`.

---

## Track Default

Give an audio track a stored treatment — volume, pan, filters, EQ, delay, reverb and the rest of its
parameters — and every new recording on that track starts from it. Set up the snare track once, and every snare
loop recorded on it sounds right from the first take. This applies to audio tracks only.

It has no toggle: nothing changes until you make a track default, and a song without one plays exactly as
before.

**Set it.** Dial in a clip on the track until it sounds right. Open that clip's settings: hold its pad in grid
view, or its status pad in the rows layout, and press SELECT. Choose **Make track default**.

**What it does.** When an empty clip on the track starts recording, its parameters are set to the track default,
whichever clip it was copied from. Clips that already hold audio are never changed, and nor are overdubs
recorded onto existing audio. Use Track Edit to bring existing clips in line.

**Clear it.** The same menu shows **Clear track default** while the track has one.

**Saving.** The default is saved with the song. Firmware without this feature ignores it and loads the song
normally.

**Known limits.**
- Audio tracks only, for now.
- Parameters only. The track's input, mode and FX types are already shared by every clip on the track.
- An automated parameter on the recording clip keeps its automation.
- A track still disappears when its last clip is deleted, default and all, the same as stock. Keep one clip on
  each track you want to keep.

Source: `AudioOutput::trackDefault` and friends in `src/deluge/processing/audio_output.{h,cpp}`, applied in
`AudioClip::beginLinearRecording`, menu in `src/deluge/gui/context_menu/clip_settings/clip_settings.cpp`. Design
notes in `TRACK-DEFAULT-FEASIBILITY.md`.

---

## Harmonize

Nominate one track as the **chord track**. Any other clip you switch `Follow Chords` on for stops
playing its written pitches and plays its written *shape* instead — each note becoming a tone of
whatever chord is sounding on that track right now. Rhythm, phrasing and contour are exactly as
written; only the pitches move, and they move with the progression. The OXI One's `HARMO` scale,
by another name.

Off by default. A song with no chord track set sounds identical either way, so turning it on cannot
change an existing song.

**Setting it up.** `SETTINGS > COMMUNITY FEATURES > Harmonize` once per unit. Then per clip:
`MIDI FX > Enabled = On`, `MIDI FX > Harmonize > Chord Track` (song-wide — set it once) and
`MIDI FX > Harmonize > Follow Chords = On`. On 7SEG the page is `CHRD` and its entries are `FOLO`
and `TRAC`; `HARM` is not available because M is one of the letters that display cannot form.

### How a note is chosen

Each note's **degree** in the song's key picks a tone of the chord: the key root plays the chord's
lowest note, the next scale step plays the next chord tone, and past the top of the chord it wraps
back to the bottom. That tone is then placed in the **register of the note you wrote** — shifted by
whole octaves until it sits within a tritone of it.

Degree picks the tone, the written note picks the octave. They are two separate questions and the
feature answers them separately; the arithmetic that tries to answer both at once is what went wrong
the first time, below.

The consequence worth knowing: **a phrase keeps its register, and a wide phrase may not keep its
contour.** Seven scale degrees map onto three chord tones, so a line climbing an octave in scale
steps would have to climb 7/3 octaves to keep both. It cannot do both. Register won, because a
bassline that climbs into the melody is a worse fault than a leap that folds. Phrases moving inside
about a fifth — most melodies, most basslines — come back exactly as written.

| What you wrote | What you get |
|---|---|
| A stepwise run | An arpeggio of the chord |
| A leap of an octave | A chord tone near where you wrote it; the leap may fold |
| A note outside the song's key | Snapped to the nearest chord tone, in its own register |
| Two rows landing on one pitch | The second moved to a neighbouring chord tone |

**Corrected on 2026-09-14.** Until then this arithmetic was wrong in a way that only showed on some
material, which is why it survived a hardware pass. The chord's pitches are already absolute, and
the code added the written note's octave-above-the-key-root to them a second time — register counted
twice. With a song key rooted at its default, a line written across C4–B4 over a C3 triad came out
at the very top of the keyboard. A running degree carried across octaves then stretched wide lines
further still. If you have a song that sounded wrong under Harmonize, it will sound right now.
`HARMONIZE-REGISTER-FIX.md` in the `wt-midifx` worktree is the precise before and after, for all
three attempts it took.

### The chord track

An **Output**, not a clip, stored by name on the song — so it follows clip launches on that track,
and survives adding, deleting and reordering tracks. Renaming it unhooks it, which is the price of
storing a name instead of a position; an unnamed track cannot be chosen at all.

- **A muted chord track still steers.** Mute the chords so nobody hears them and the followers carry
  on. This is the top standing complaint about the OXI's own implementation, and it falls out of
  reading written notes rather than live state.
- **A muted row leaves the chord** — a row you muted is a note you took out.
- **Between chords the last chord is held**, so a phrase does not snap back to its written pitches
  for the length of a rest.
- Up to eight notes count as a chord. More than that is not a chord.

### Two decisions that carry the feature

**The pitch is latched at note-on.** The note-off path recomputes nothing: if it worked the pitch out
from the chord a second time, a chord that moved during the note would send an off for a note the
instrument is not holding and leave the real one ringing. One `int16_t` on the note row covers it,
and it is why a held note is not re-pitched mid-note.

**The chord is read from the source's written notes at its current position**, not from its live
`sequenced` flags. Live state is free to read and wrong twice: clips are processed in list order
within a tick, so a follower processed before the chord clip reads *last* tick's chord and puts a
wrong note on the downbeat of every chord change — intermittently, depending on track order — and a
muted chord track stops steering.

### What it does not touch

- **Kits.** A kit row is a drum rather than a pitch, so the page hides itself there.
- **The written clip.** Nothing is rewritten, and the grid, note editor and keyboard keep showing
  what you wrote rather than what is sounding.
- **Anything downstream.** The re-pitch happens before `MelodicInstrument::sendNote`, so the
  arpeggiator and the rest of the MIDI FX chain receive the harmonised note for free.

### Known limits

- One chord track per song. Two progressions at once would need the source per clip.
- A held note is not re-pitched mid-note (above).
- A renamed chord track unhooks silently.
- 1.3 only. It reads the song's key through the 1.3 scale objects, which 1.2.1 does not carry in the
  same shape.

Source: `src/deluge/modulation/midi_fx/harmonize.h` (the mapping, pure functions) and
`chord_source.{h,cpp}` (the chord read, its cache and the held chord); the menus are
`src/deluge/gui/menu_item/midi_fx/harmonize.h`. The four send sites that read the latch are
`InstrumentClip::sendPendingNoteOn`, both halves of `NoteRow::playNote` and
`NoteRow::check_for_note_still_sounding`; `NoteRow::getNoteCode()` is deliberately untouched, since
rendering and editing depend on it meaning "the note as written". Design notes in
`HARMONIZE-FEASIBILITY.md`.


---

## Strum

A chord stops landing as a block and lands as a sweep: its notes fire one after another across a few
milliseconds, low to high or high to low, the way a hand crosses guitar strings. Every pad-drawn
chord on a Deluge is perfectly simultaneous, which is the one thing no played instrument ever is.

The OXI One's timed strum rather than the KeyStep's gesture — you set a spread and every chord in the
clip strums, hands off, while your hands are somewhere else.

Off by default: a clip with `Spread` at 0 plays exactly as it always did, so turning the feature on
cannot change an existing song.

**Setting it up.** `SETTINGS > COMMUNITY FEATURES > Strum` once per unit. Then per clip:
`MIDI FX > Strum > Spread` above 0. On 7SEG the page is `STRM` and its entries are `SPRD`, `DIR` and
`CONT`.

### The three controls

| Control | Range | What it does |
|---|---|---|
| **Spread** | 0–24 ticks | The gap between successive notes. 0 is off. |
| **Direction** | Up / Down / Random | Which end of the chord goes first. Random reshuffles every chord. |
| **Contour** | −50% to +50% | How much the velocity has changed by the last note of the sweep. |

There are 48 ticks to a quarter note, so one tick is about 10 ms at 120 bpm: a Spread of 1 across a
three-note chord is a credible flam, and the top of the range is a slow harp roll. It is
**tempo-relative** — the strum widens as the tempo drops. Most gear that offers this behaves the same
way, and going absolute would mean carrying a second timing frame through the sequencer.

Contour is the difference between "spread out in time" and "sounds strummed". The first note keeps
its written velocity and the rest are interpolated to the value you set, so a three-note chord and a
six-note chord finish in the same place rather than the long one fading to nothing.

### Clip-wide, kits included

Unlike Velocity, Gate, Repeat, Delay and Spice, Strum is **not per drum**. A flam is a property of
the group of drums hitting together, not of any one of them, and two drums holding different spreads
is not a coherent question. So there is one set of values per clip, `AFFECT ENTIRE` means nothing
here, and on a Kit the page stays visible and editable with no drum selected.

On a Kit the sweep runs in **row order** — what you see top to bottom on the pad grid — because pitch
means nothing there. A strummed kit chord is a flam, and a good one.

### What it composes with

- **Spice** thins the chord and the strum then spans the survivors, so the same chord strums
  differently each variation.
- **Harmonize** moves the pitches and the strum follows the new voicing.
- **Play FX** shifts the whole strum as a unit.
- **The arpeggiator** switches Strum off for that clip. The arp plays a chord one note at a time from
  a set of held notes it builds as they arrive, so spreading the note-ons does not spread its output —
  it feeds the arp a chord that grows from one note to four and changes the pattern. Standing down is
  less surprising than that.

### Notes keep their length

A strummed note ends where it would have ended plus its own delay, so it keeps every tick of what you
wrote and the chord releases raggedly — what a piano does with a rolled chord, rather than a damped
guitar. A note is never delayed past its own end: the offset is clamped to one tick short of the
note's length, because a note-off falling due before its note-on is a hung note.

### What it does not touch

- **The written clip.** Nothing is rewritten. Turn Spread back to 0 and playback is exactly what it
  was, and recording over a strummed clip records what is written, not what you heard.
- **Drones.** A note filling its whole row is left alone, the same way Spice leaves it alone: that
  note deliberately does not get stopped at the loop point, so there is no note-off to move with it.
- **Single notes.** A chord of one is a chord of one, which is what makes the feature inert on
  melody lines.

### Known limits

- Alternate (up then down) and Inward/Outward are not in this version; Up, Down and Random are.
- Resolution is one tick. At 160 bpm a tick is about 7.8 ms, so a three-note strum at Spread 1 spans
  ~16 ms. Finer than that would need sub-tick placement, which the note path does not offer.
- Nothing on screen shows a strum. The clip still holds the chord you wrote; the only confirmation is
  what you hear.

Source: `src/deluge/modulation/midi_fx/strum.h` (the ordering, offsets and contour, as pure
functions), the deferral in `InstrumentClip::processCurrentPos` and its queue alongside it, the
latched offset read in `NoteRow::processCurrentPos`, and the menus in
`src/deluge/gui/menu_item/midi_fx/strum.h`. Design notes in `STRUM-FEASIBILITY.md`.

---

## Generate

**The one MIDI FX that writes.** Everything else in this list transforms notes on their way out and
hands you back exactly what you wrote the moment you turn it off. Generate replaces the notes in the
row, goes through the action log like any other edit, and UNDO is how you get back. That is the
point of a generator rather than an oversight: you want to keep editing what it wrote.

Rhythm and line are two separate questions here, because on this machine they have to be. A melodic
note row *is* one pitch, so notes written into one row can only ever be a rhythm at one pitch. A
tune means writing across rows, creating the ones that do not exist yet, and logging the lot as a
single action so one UNDO takes the whole line back out.

So Density, Seed and Gate decide **when** notes happen, and Range and Leap decide **what pitch** each
one takes. Independent on purpose: turn Range up and a tune appears over the groove you already had;
turn it back to 0 and the groove is still there, unchanged.

**Setting it up.** No community feature switch — it is always present. Pick the target row *first*,
because that is where it writes: on a Synth, MIDI or CV clip the row whose **audition pad you last
tapped**; on a Kit the **selected drum**. Then `MIDI FX > Generate`. On 7SEG the page is `GENR` and
its entries are `DENS`, `RANG`, `LEAP`, `SEED`, `GATE` and `GO`.

### The five controls, and the command

| Control | Range | What it does |
|---|---|---|
| **Density** | 0–100% | How likely each step is to carry a note. Default 30. |
| **Range** | 0–24 degrees | How far above the target row the tune may climb. **0 is one row, one pitch.** |
| **Leap** | 0–100% | How often the line jumps rather than stepping to the next note of the scale. Default 25. |
| **Seed** | 0–99 | Which pattern. Not a dice roll — below. |
| **Gate** | 5–200% | Note length as a percentage of one step. Default 100. |

Nothing happens until you choose **Generate now**. Turning any of the five changes nothing on its
own, so the page can be set up before you commit to it. The display tells you how many notes landed.

### Seed is an index, not a dice roll

Seed 7 is always the same part, on any build, forever — the same idea as Spice's Dice and for the
same reason. A generator whose knob draws fresh randomness is a slot machine: you find something you
like, turn the knob, and it is gone. Here, turn Seed, press Generate, listen; if you preferred the
one two turns back, turn back to it and press Generate again. It is the same part. Nothing is stored
per note — the pattern comes out of the hash every time it is asked for.

The placement roll is keyed on **tick position, not step index**, so the decision for a given point
in the bar is the same whatever the zoom was. Zooming in offers new candidate steps *between* the
ones already there rather than reshuffling the lot, so generating again at the same Seed refines the
pattern instead of replacing it.

### The line

Above Range 0 the generator writes across rows, creating the ones it needs, in degrees of the song's
key — so the line is in key by construction rather than by correction, and it follows the song if
you change the scale later. Range 7 is roughly an octave of a normal scale.

Leap is the control that most decides whether the result sounds like a melody or like noise: near 0
it is a scale run, near 100 a scattered arpeggio, and the middle is where tunes live.

A line that reaches the top of its range **reflects off it rather than clamping to it**. Clamping is
the obvious thing and it is audibly wrong — the line piles up on the top note and repeats it for as
long as the walk keeps pushing upward, which reads as the generator having got stuck. Reflection
turns that push into a descent, which is what a melody does.

### Drums

Leave **Range at 0**, and on a Kit it is ignored anyway: a kit row is a drum rather than a pitch, so
a line climbing through rows would walk across your kit. The useful controls there are Density, Seed
and Gate. The seed is combined with the row, so the same Seed number gives a *different* pattern on
each drum — tap a drum, set Density, generate, turn Seed until you like it, then tap the next drum.
The kit does not stutter as one block.

### What it composes with

- **Everything downstream.** The notes it writes are real notes: nudge them, re-length them, change
  their velocity, spice them, strum them, harmonize them. Generate is a starting point, not a mode
  you are stuck in.
- **Generating twice replaces rather than stacks.** The second run clears the rows the first one
  used, so the Seed number always means what it says.
- **UNDO takes one whole generation back out** — every row of it — and a second UNDO the one before.

### What it does not touch

- **Rows it did not write into.** Only the target row and, above Range 0, the rows the line needs.
- **The song's key.** It reads it; it never sets it.
- **Anything at Density 0** — the command refuses rather than silently emptying the row. Clearing a
  row is what CLEAR is for.

### Known limits

- It writes **upward** from the target row only. There is no downward range.
- `Select a drum or note row first` (`ROW` on 7SEG) means it could not work out where to write: tap
  a drum pad on a kit, an audition pad on a synth. It also refuses at Density 0, and it works only
  from clip view — not song view, not the arranger.
- The step grid is the zoom you are looking at, so zoom in for a finer pattern.
- Gate over 100% is clamped at write time to stop where the next note starts, so turning it up makes
  a sparse pattern legato and leaves a dense one alone.

Source: `src/deluge/modulation/midi_fx/generate.h` (the placement roll and the degree walk, as pure
functions) and the menus and the command itself in
`src/deluge/gui/menu_item/midi_fx/generate.h`. `GENERATE-HOWTO.md` in the `wt-midifx` worktree is
the one-page driving guide; design notes in `GENERATIVE-FEASIBILITY.md`.

---

## Spread

Velocity, gate and octave variation on notes the sequencer already holds — three knobs that stop a
written part being mechanically identical every time round. Nothing is written into the clip: turn
them back to 0 and the phrase is exactly what you wrote, the same contract Spice, Strum, Harmonize
and Stochastic all keep.

**A name clash worth knowing about.** The arpeggiator's own Randomizer page carries these same three
names already, and every one of its parameters does nothing with the arp off — they are read only by
the arp. Reusing the names here is deliberate, so that someone who knows that page knows this one,
with one difference: there, Spread Octave is a *range*; here Octave is a *chance*.

**Setting it up.** No community feature switch. `MIDI FX > Spread`. On 7SEG the page is `SPRD` and
its entries are `VELO`, `GATE` and `OCTV`. Each control is its own switch — 0 is off — and
`MIDI FX > Enabled` is not involved, because these live on the clip rather than in `MidiFxSettings`.

### The three controls

| Control | Range | What it does |
|---|---|---|
| **Velocity** | 0–64 steps | How far either side of the written velocity a note may land. |
| **Gate** | 0–90% | How far either side of its written length a note may run. Both ways. |
| **Octave** | 0–100% | How often a note is thrown a whole octave, up or down. |

Velocity is **absolute rather than proportional**, which is the useful choice on a part that already
has written accents: at 10, the quiet ghost notes and the loud accents both wander by up to ten,
rather than the accents wandering four times as far and swamping the shape you wrote.

Gate varies both ways, unlike Spice's, which only ever shortens. It tops out at 90 rather than 100
so the bottom of the knob stays useful — a note that could reach 0% of its length is a note that can
vanish.

### Two spreads and a probability

That Octave is a chance and the other two are spreads is not sloppiness. Velocity and gate are
continuous, so "a bit more, a bit less" means something and every note should differ slightly. An
octave is quantised and coarse: at ±1 there is no small amount — a note is displaced or it is not.

### No Dice

Spice has a Dice because Spice makes patterns and browsing different rolls of a pattern is the point
of it. This is an amount, and nobody auditions variations of humanisation, so the roll is a plain
hash of row, position and salt with no variation index. It repeats bar after bar, survives
stop/start and save/load with nothing stored, and comes out identical on both cores.

Each of the three has its own salt, so the three decisions for one note are independent. Sharing a
roll would tie them together and every loud note would also be a long one.

### Hidden on Kits

Both of the things it offers are already answered there. A kit row's velocity is randomisable through
the mod matrix by patching master volume to Random, and an octave means nothing on a row that is a
drum rather than a pitch. A page showing three controls of which one duplicates the matrix and two do
nothing is worse than no page.

### What it composes with

- **Harmonize.** Octave runs **after** Harmonize, never before. Harmonize derives a pitch from a
  scale degree, so displacing first would have that mapping throw the octave away. Displacing after
  keeps the note a chord tone, because an octave of a chord tone is still a chord tone.
- **Stochastic.** Same order, for the same reason — Spread is last in the pitch chain.
- **Spice.** Spice decides whether a note sounds at all; Spread varies the ones that do.
- **Strum.** The strum spreads the chord in time and Spread varies the notes inside it.

### What it does not touch

- **The written clip.** Nothing is rewritten, and the grid and keyboard keep showing what you wrote.
- **Velocity 0.** The floor is 1, because velocity 0 is a note-off on MIDI — a spread that reached
  it would be a hung note wearing a disguise.
- **One-tick notes.** Left alone. A length that rounds to 0 ends on the tick it began, which the
  sequencer reads as "already over" rather than "very short", and that is how a row goes silent for
  reasons nobody can find.
- **Notes at the edge of the keyboard.** An octave displacement that would leave the note range is
  **not applied at all**. Clamping would pile notes onto one pitch, which sounds like a fault, and
  dropping would lose a note the phrase expects — so near the bottom of a bassline the down-jumps
  simply do not happen.

### Known limits

- A lengthened note is clamped against the next note in its row before the note-off is scheduled.
  Two overlapping notes in one row is not a state the note vector is allowed to be in, so on a dense
  row Gate has less headroom upward than the knob suggests.
- Nothing on screen shows the variation. The only confirmation is what you hear.
- No Dice, by design (above), so there is no way to browse alternative humanisations — only more or
  less of it.

Source: `src/deluge/modulation/midi_fx/spread.h` (all three, as pure functions), the velocity and
octave hooks in `InstrumentClip::sendPendingNoteOn` and `NoteRow::processCurrentPos`, the gate hook
beside the note-off scheduling in `note_row.cpp`, and the menus in
`src/deluge/gui/menu_item/midi_fx/spread.h`. Design notes in `SPREAD-FEASIBILITY.md`.

---

## Stochastic

**The pitch half of Spice.** Spice locks the pitch and varies the gate; this locks the rhythm and
varies the pitch. The clip plays the steps, lengths and accents you wrote, but some of the notes land
a scale degree or two away from where they sit on the grid — and which notes, and how far, is a
variation you can browse and come back to. The OXI One's second generative mode.

Nothing is written into the clip. Amount 0 is the path the sequencer took before this existed.

**Setting it up.** No community feature switch. `MIDI FX > Stochastic`. On 7SEG the page is `STOC`
and its entries are `AMNT`, `WDTH` and `ROLL`. Amount is the only switch — `MIDI FX > Enabled` is
not involved, for the same reason as Spread.

### The three controls

| Control | Range | What it does |
|---|---|---|
| **Amount** | 0–100% | The chance that a given note is redrawn at all. 0 is off. |
| **Width** | 1–7 degrees | How far it may go when it is, either side of the written note. Default 2. |
| **Dice** | 0–99 | Which variation. **This is Spice's Dice**, shared rather than duplicated. |

**Two controls rather than one, because "how often" and "how far" are different questions.** A single
combined knob can only ever give a constant shimmer at low settings; it can never give "my part,
mostly as written, with the occasional departure", which is the setting people actually want.

At Amount 100 every note moves. The offset is drawn so that it is **never zero** — a flat draw over
−width…+width would include 0, so one fired note in every `2×width+1` would not actually move and the
knob would overstate itself by about one note in five at Width 2.

Width 1 is reachable and is meant to be: every note moves by exactly one degree, up or down. It
reads as a deliberately wandering line rather than as a fault, which is why the minimum is 1 and not
2. Width 7 is an octave of a seven-note scale, which is as far as a wander can go before the contour
of the written phrase stops being audible underneath it.

### Degrees, not semitones

The wander is an offset in degrees of the song's key, so the result is in key by construction rather
than by correction, and it follows the scale when you change it. A version that drew semitones and
then snapped would be in key only by accident, and would bunch onto whichever pitches the snap
happened to favour.

### Where it sits in the chain

**written → Stochastic → Harmonize → Spread → the latch.** Stochastic picks a degree, Harmonize maps
that degree onto a chord tone, Spread may then throw it an octave. The order is the whole design:
wander *after* Harmonize and a generative clip simply plays notes that are not in the chord.

There is no correction between the stages. An earlier version folded the harmonised result back
toward where the written note would have gone, because Harmonize was amplifying a two-degree wander
into a jump of one or two octaves. That amplification turned out to be a bug in Harmonize itself and
was fixed at source, so the correction is gone — and with it the cap it was quietly putting on the
top of Width's range.

### What it composes with

- **Spice.** The obvious pairing: Spice thins the rhythm, Stochastic moves the pitches, one Dice
  browses both.
- **Harmonize.** Every wandered note still belongs to the chord. That is the pair worth testing.
- **Spread.** Octave-displaced wandered notes are still chord tones under Harmonize.
- **The arpeggiator.** It arpeggiates the wandered notes with no effort at all, because the pitch is
  decided upstream of the arp.

### What it does not touch

- **Notes outside the song's key.** A note with no scale degree has nothing to add an offset to, so
  it never moves, at any Amount. Moving it by "two degrees" would mean moving it by an arbitrary
  interval, and the chromaticism was written on purpose. (Harmonize's fallback for the same case is
  to snap to the nearest chord tone, which is right for a harmoniser and wrong here.)
- **Kits.** A kit row is a drum, so a wandering note code would play a *different drum*. The page
  hides itself there, as Spread's does. The kit reading of this idea — wander to a neighbouring drum
  — is a genuinely good feature and a separate one.
- **Notes at the edge of the keyboard.** A wander that would leave the note range is **declined, not
  clamped**, so a bassline sitting on the floor simply stops making its downward jumps rather than
  piling them onto the lowest note.
- **The written clip.** Nothing is rewritten. Amount back to 0 and playback is exactly what you
  wrote.
- **A held note.** The pitch is latched at note-on and read back at note-off, so it is not re-drawn
  mid-note and cannot hang.

### Known limits

- **The Dice is shared with Spice.** That is the better user model — one number meaning "which
  variation am I hearing" across the group — but it cuts both ways: browsing variations here also
  re-rolls Spice's gates, and the Stochastic page's Reset zeroes the Dice along with Amount and
  Width.
- A song with no scale — or a clip whose key the song cannot resolve — has nothing to wander in, and
  the feature stands down rather than guessing.
- Nothing on screen shows the wander. The grid keeps showing what you wrote.
- Width is in scale degrees, so its size in semitones depends on the scale. Seven degrees of a
  pentatonic is not seven degrees of a major scale.

Source: `src/deluge/modulation/midi_fx/stochastic.h` (the arithmetic, pure and free of the firmware
so the header test can evaluate it), `pitch_source.{h,cpp}` (the chain order and the one place the
key is read), and the menus in `src/deluge/gui/menu_item/midi_fx/stochastic.h`. Design notes in
`STOCHASTIC-FEASIBILITY.md`; `STOCHASTIC-EAR-TESTS.md` is the hardware checklist and
`STOCHASTIC-REVERSE.md` the exact before/after for backing the chain change out.

---

## How the gating works

`src/deluge/gui/menu_item/runtime_feature/gated.h` wraps any `MenuItem` so it disappears from its parent
when a feature is off. Gating a menu's children is usually enough to make its container vanish too,
since `HorizontalMenu::switchHorizontalMenu()` already skips any menu whose paging reports no relevant
items; containers are gated as well so they also drop out of ordinary submenu listings.

Three items are gated in place rather than wrapped, because they are `final`: the two EQ param classes,
where only the `LOW_MID` and `HIGH_MID` bands are hidden, and the Clouds `Freeze` toggle, which already
had an `isRelevant()` for the engine-off case that the check folds into.

`AUX Sends` is gated differently again, and more cheaply: every menu class in
`cv_output/routing.h` is `final`, but they all already asked one shared predicate,
`cvOutputsAvailable()`, before showing themselves. That call became `auxMenusVisible()`, which is the
hardware check *and* the feature check, so one helper takes the whole feature out of the menus — the
sends on all three roots and `SETTINGS > AUX` alike.
