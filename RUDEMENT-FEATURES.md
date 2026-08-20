# Rudement features

The additions in this fork that are not part of official or upstream community firmware, and the
Community Features toggles that control them.

This file is deliberately fork-local. The upstream page at
`website/src/content/docs/features/community_features.mdx` is left alone: it is shared with
SynthstromAudible, it changes on nearly every merge, and anything written there would ride along in a
PR back upstream.

Every feature below can be switched off from `SETTINGS > COMMUNITY FEATURES`. **Doing so hides the
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
