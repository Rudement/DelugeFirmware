# Applying branches to this fork

One document, replacing the eleven `HOW-TO-APPLY-*.md` files. Those are parked in
`_to_delete/applied-docs/` if you ever want the long form; everything below is
what survived — the steps that still matter and the things nobody has tested yet.

---

## The rules that apply to everything

**Run git from Windows, not from a Cowork sandbox.** The FUSE mount permits file
creation but denies deletion, so git takes locks it cannot release.
`docs/DEVELOPMENT-NOTES.md` on `beta-1.3` has the detail. Every stale
`index.lock` and stranded `tmp_pack_*` in this repo came from ignoring it.

**`dbt nuke` when you cross firmware lines.** The 1.2.1 line is toolchain **v16**,
the 1.3 line is **v22**. CMake silently keeps compiling with whichever toolchain
configured the cache first, so a stale `build/` gives you a binary from the wrong
compiler with no warning.

```
git checkout <branch>
dbt nuke                 (only when crossing v16 ↔ v22)
dbt build release -m -t beta     (1.3)
dbt build release -m             (1.2.1)
```

**Nothing built in a sandbox is flashable.** Compile checks done there prove the
code type-checks; they do not prove codegen. The Windows build with the official
toolchain is always the binary that goes on the Deluge.

**Housekeeping that is still outstanding:**

```
git gc --prune=now
```

`.git/objects/pack/` accumulated ~672 stranded `tmp_pack_*` / `tmp_idx_*` files
(~6 MB) from sandbox writes, and more have been added since. `_to_delete/` holds
about 510 MB of parked scratch, bundles and old builds — delete it from Windows.
Two tracked leftovers can also go: `heat.hpp.stranded` and
`PIPELINE-TEST.md.stranded`, neither of which anything includes or compiles.

---

# Not yet applied

## Chroma — the Harmonic layout and Harmonic Brush

The only thing in this repo still waiting to be fetched. `chroma.bundle` carries
both lines; `chroma-cody.bundle` holds Cody's originals for reference.

```
cd C:\Users\Public\Cowork\Deluge\DelugeFirmware
git bundle verify chroma.bundle
git fetch chroma.bundle feat/chroma-13:feat/chroma-13
git fetch chroma.bundle feat/chroma-12:feat/chroma-12
git fetch chroma.bundle integration/chroma-13:integration/chroma-13
git fetch chroma.bundle integration/chroma-12:integration/chroma-12
git checkout integration/chroma-13
```

Four branches. The two `feat/` ones are Chroma on its own — `feat/chroma-13` on
`base-13` (`134d000f`), `feat/chroma-12` on `base-12` (`a2e333b9`), three commits
each: support layer, the layout, the Brush. The two `integration/` ones are those
merged into the tips you actually flash, so **one binary carries everything**.

| Branch | = | Contains |
|---|---|---|
| `integration/chroma-13` | `feat/aux-sends-oled-kitsplit-13` + Chroma | AUX on OLED, Kit Split, Clouds, Plaits, EQ, Gristleizer, Sear, Chroma |
| `integration/chroma-12` | `feat/aux-sends-oled-kitsplit-12` + Chroma | the same minus Clouds, plus the EQ Hz/dB readout |

**All four are build-verified**, unlike everything below: the real `dbt` toolchain,
v22 on the 1.3 line and v16 on 1.2.

| | text | data |
|---|---|---|
| `feat/chroma-13` | 1 658 598 | 67 916 |
| `feat/chroma-12` | 1 293 452 | 27 808 |
| `integration/chroma-13` | 1 966 952 | 91 100 |
| `integration/chroma-12` | 1 493 428 | 49 112 |

Both integration binaries were then checked for every feature's strings — all ten
toggles on 1.3, all seven on 1.2.1.

### One thing the merge got wrong on its own

Worth knowing because it will happen again to anything that appends a toggle.
Merging Chroma into the 1.2.1 tip, both sides had appended to `subMenuEntries` at
the same place, and git kept only one side's version of that region — silently
dropping `&menuChordBrush` and `&menuRetrospectiveCapture` while their enum entries
and declarations survived. 31 enum entries against 28 array entries, which is
precisely the trailing-`nullptr` freeze. **Count after every merge that touches
that array**; the compiler will not tell you.

Extracted from Cody's squashed `chroma-on-main` (`44d5aeac`), leaving out the SD
recency clock, autosave/recovery, song-date repair, the recents sort, and his
`CMakeLists.txt` changes — a version bump to 1.3.41 and `ENABLE_SYSEX_LOAD`
flipped **on**, which would have collided with `feat/version-label-13`. Two bugs
in his branch are fixed here: the l10n strings he never added (every new toggle
rendered blank) and `kNonTopLevelSettings`, which freezes the device if wrong.

Full operating manual: **`CHROMA-MANUAL.md`**.

### Confirmed on hardware — 22 August 2026

`integration/chroma-13` at `8b1cbe86` boots and runs. Verified on the device:

- **Both toggles present** — `Chord Brush (BRSH)` and `Retrospective Capture (RCAP)`
  under `SETTINGS → COMMUNITY FEATURES`, and the list scrolls to the bottom
  without freezing, which is the check `kNonTopLevelSettings` exists for.
- **Chord packs load.** A well-formed `CHORDS/*.chordpack` appears after the
  built-ins on the first PROG press, and a deliberately malformed one alongside
  it neither loads nor takes anything else down with it.
- **The spring-loaded voicing dial works.** Holding a chord pad and turning the
  vertical encoder walks the voicing and reports `V1`, `V2`… as it should,
  rather than scrolling the iso panel.

Everything else in the layout is still unexercised — the overlays, the Brush,
retrospective capture, LEARN, and the whole 1.2.1 branch.

---

# Applied — what is on the branches, and what is still untested

## AUX Sends

A port of sticknobills' `AUX-Sends-1.0.1` — an assignable audio send bus on the CV
outputs — carried in so it sits alongside Clouds, Plaits, Gristleizer, Sear, the
four-band EQ and Kit Split. Every conflict during the port was resolved by keeping
both sides.

| Line | Branch | Notes |
|---|---|---|
| 1.3 | `feat/aux-sends-oled-kitsplit-13` (`0d1d9e51`) | the current tip — AUX + OLED + Kit Split + toggles |
| 1.2.1 | `feat/aux-sends-oled-kitsplit-12` | five commits on Chopin, same feature set minus Clouds |

**What it does.** An AUX submenu on the Kit-global, Audio Clip and Sound roots:
`MAIN` (send this Clip/Kit/Sound to the CV sockets) and `CV Send`, or `CV1 Send` /
`CV2 Send` when Split is on — real automatable, LEARN-able params. Under
`SETTINGS → AUX` (kept separate from `SETTINGS → CV`, which is pitch voltages):
`CV1 Level`, `CV2 Level`, and `Stereo Split`, which decides whether the sockets are
one stereo destination or two mono aux buses.

**Flash offset differs by line.** The CV stereo-split flag lives at `buffer[198]`
on 1.3 (upstream's own offset, which was free) and at **178** on 1.2.1, where this
fork's settings already reach through 177. Song files are unaffected — this is the
separate persistent-settings buffer.

### The OLED work — the risky part

The DAC and the display are on the same RSPI channel (`SPI_CHANNEL_CV` and
`SPI_CHANNEL_OLED_MAIN` are both 0). AUX Sends installs a self-linking DMA
descriptor that feeds that channel continuously, which would lock the display out
entirely — hence the original gate that switched the menus off on OLED models.

The fix separates menu visibility from socket capability, then makes the stream
give the bus up on demand: `P6_1` is re-muxed to SSL00 while the stream holds the
bus and parked high as GPIO when it hands over; a resume rebuilds a link descriptor
from the DMA's live `CRSA_n` rather than restarting the ring; and a still screen
never transfers, so it never interrupts.

**What it costs:** the sockets hold their last voltage while the display holds the
bus, and a resume forces a phase resync rather than letting the ±0.3% rate loop
recover. Expect silence while the screen is still and a tick per redraw while it is
not — menu scrolling is the worst case. Separately, **note voltages are now dropped
on both display models while a stream runs**, not just 7SEG. Audible and intended:
the stream drives both DAC channels, so there is no socket left for pitch.

### Still untested on hardware, in order of what a failure would tell you

1. **Does the display survive a send being turned up?** If the OLED freezes, the
   bus is not coming back — same symptom class as issue #3670. This is the
   correctness question; everything else is tuning.
2. **Is there audio at the socket at all?** Silence or noise points at the
   chip-select: the stream muxes P6_1 expecting SSL00, a path that has only ever
   run on 7SEG boards.
3. **How bad is the ticking?** Sustained sound, send up, scroll a long menu name.
   A still screen should be clean.
4. **Turn a send up while text is already scrolling** — the path where
   `cvStreamStart()` finds the bus busy and comes up yielded.
5. **Route away and back several times** — confirms the SPI restore, including the
   corrected `SPBR` (it used to be hardcoded to 1, which is boot's rate for 7SEG's
   30 MHz request and wrong on OLED, where boot asks 10 MHz and gets 3).
6. **A CV/gate track with a send active**, to hear the note-voltage drop and decide
   you can live with it.
7. **A 7SEG build off the same branch**, to confirm nothing moved there.
8. **Load a song saved before AUX Sends.** Params are added at the end of their
   enums and song files store params by name, so nothing should shift.
9. **AUX Send by pad and by scroll**, in both Split states.
10. **Kit affect-entire and Audio Clip AUX MAIN reachability** — MAIN is only meant
    to be reachable from the arranger.

### If the crackling comes back

The build that crackled predates the five Clouds load fixes (`394af578`..`6604bc3d`),
including the one that took `Prepare()` off the audio thread. Those are on the
current tip. The bus handover is not a plausible cause with no send up:
`cvStreamYieldBegin/End` test `cvStreamRunning` first and return before touching
hardware, and the documented cost of a handover is a gap on the CV socket, not the
main outs. To narrow it: test `feat/kit-split-13` (no AUX at all); try a song with
no Clouds anywhere, remembering the toggles hide menus and nothing else; and check
whether it tracks the screen or the CPU by scrolling a menu with playback stopped.

### What the 1.2.1 port had to change

The core carried over untouched — `cv_audio_stream.cpp/.h` and
`cv_output/routing.h` were byte-identical between the lines. Four things were not:

- The shared-bus flag is `spiTransferQueueCurrentlySending` here,
  `spiBusCurrentlySending` on 1.3.
- `oled_low_level.c` is a different file: one queue with a `destinationId` per
  entry, no priority-CV path and no lost-interrupt watchdog. Simpler to hook — one
  yield at the top of `sendSPITransferFromQueue()` covers frames and CV words alike.
- **`CriticalSectionGuard` did not exist**, backported verbatim from 1.3's
  `timers_interrupts.{h,c}`. 1.2.1 only had non-nesting
  `DISABLE_ALL_INTERRUPTS`/`ENABLE_INTERRUPTS`.
- **`invalidate_range_all_caches()` did not exist.** There is no L2 to clean on this
  line, so the four call sites use `v7_dma_flush_range()`. **If the L2 data cache is
  ever enabled on 1.2.1, those four sites are where it has to be revisited.**

That backport is new code in a shared header. If something unrelated starts
misbehaving around interrupt timing — gate jitter, MIDI clock, SD stalls — suspect
it first; reverting `f1c25e1b` takes it out along with the port.

Worth knowing: `feat/aux-sends-12` as it stood **could never have compiled** — it
carried the 1.3 `cv_audio_stream.cpp` unchanged against a tree missing both of the
above. Nobody had built it.

---

## Kit Split

`feat/kit-split-13` on the 1.3 line, `feat/kit-split-12` on 1.2.1. Built clean on
1.3 against `a75722db`: `+3440` text, `+224` data, no new warnings.

Ships behind a runtime flag, **off by default**, because it is destructive:
`Settings → Community Features → Kit Split`. Then on a kit clip with affect-entire
on: `Kit Global FX → Actions → Split Kit → "Split into N"`. Stepping into the
submenu is the confirmation — the label states how many kits you are about to get.

Every row with notes becomes its own kit holding one sound, with a clip carrying
only that row's notes. The source clip and kit are consumed.

| Travels | Does not |
|---|---|
| Notes, velocity, probability, iterance, fill | Kit-global wet FX — delay, reverb, filter, mod FX |
| Per-row FX — filters, envelopes, patch cables, sends, sidechain | Kit arpeggiator (it indexed drums that no longer share a kit) |
| Kit-global volume and pan (values only, not automation) | Rows with no notes — those drums go with the source kit |

Volume and pan are the deliberate exception: they are gain and position, not wet
effects that stack, and each new kit holds one drum. Carrying them means the mix
lands where it started; leaving them neutral is what would change the sound.

**It refuses** if playback is running, the clip has fewer than two rows with notes,
it is the sync-scaling clip, it has arranger instances, or there are more than 64
rows with notes. **It is not undoable** — `actionLogger.deleteAllLogs()` runs first.
Allocation is all up front, so an out-of-memory leaves the song untouched.

*Not a bug:* pressing MIDI or CV on a split track says *Clip not empty* (`CANT`).
Stock `InstrumentClipMinder` behaviour, unrelated to the split.

### Still untested, in order of likelihood to bite

1. **A kit where drum order does not match row order**, and one with empty rows.
   The nastiest bug found in review only appears when the drum being moved is not
   the last in the kit's list — the happy path hides it completely.
2. **Row FX survive.** Distinctive filter + delay send on one row, split, confirm.
3. **Kit FX do not survive but level and pan do.** Affect-entire delay on the
   source, volume off centre, split: new kits dry, at the source's volume.
4. **Every new track is audible.** A moved drum can end up in nobody's render list
   and be silently muted forever.
5. MIDI and gate rows mixed with sample rows; a 20+ row kit; affect-entire on and
   gold knobs lit on every new track; save, reload, confirm.

**Known and deliberate:** `deleteNoteRow` on the throwaway clones calls
`removeDrumFromKitArpeggiator` against the *source* kit with clone-derived indices.
A no-op while the kit arpeggiator holds no notes, which the playback-stopped guard
guarantees. **If that restriction is ever lifted, this has to be dealt with first.**

`KIT-SPLIT-FEASIBILITY.md` explains why drums are moved rather than copied and why
suppressing kit FX is an active step. `KIT-MERGE-DESIGN.md` covers undo.

**1.2.1 differences**, all forced by the API: no `getDrumName()` so MIDI/gate drums
contribute no name and come out `SPLIT`, `SPLIT2`…; `storageManager.createNewInstrument`
as a global rather than a namespace; `existsOnCard` rather than `mightExistOnCard`;
and the `Actions` submenu had to be created, since 1.2.1 has no
`kitGlobalFXActionsMenu`. Front-panel path is identical.

---

## Clouds

Two branches: `feat/clouds-parasites-13` vendors Mutable's Clouds DSP byte-for-byte
(MIT) and splits Plaits' stmlib into a shared `dsp/stmlib/`;
`feat/clouds-fx-13` wires it up as a standalone effect at the head of the Song FX
chain — mode selector (Off / Granular / Stretch / Delay / Spectral / Oliverb /
Resonestor), a Freeze toggle, nine automatable params. Off is the default and
allocates nothing. Merged into `beta-1.3` via `integration/beta-13-clouds`.

**`CLOUDS-STATUS.md` is the file to read** — what works, what doesn't, and what not
to rediscover. `src/deluge/dsp/clouds/PROVENANCE.md` has the vendoring rationale
and the measured resampler SNR.

Two things still worth measuring on hardware:

1. **RAM.** `kMaxNumUnpatchedParams` is shared between Sounds and GlobalEffectables,
   so nine more global params grow *every* param manager by ~576 bytes, Sounds
   included. If that hurts on a big song, the fix is a Clouds-only param array.
2. **`.text` / CPU.** Clouds is reachable now, so `--gc-sections` no longer strips
   it. The linker-map number the feasibility doc asked for is finally measurable.

*Note on the Heat/Sear merge:* `beta-1.3` renamed Heat to Sear and made it
match level automatically (`a0da0808`); the Clouds line predated that, so the merge
took Sear and deleted Heat's implementation. `heat.hpp.stranded` is the leftover.

---

## Plaits, the EQ, Gristleizer, Sear, and the toggles

On the 1.3 line all eight Rudement toggles should appear under
`SETTINGS → COMMUNITY FEATURES`: ROUNDED CORNERS, KIT SPLIT, 4-BAND EQ, PLAITS,
CLOUDS, GRISTLEIZER, SEAR, AUX SENDS. On 1.2.1 there are five: KIT SPLIT, EQ4,
PLTS, GRIS, SEAR.

All default **On** except Kit Split, and all of them **hide menus and nothing else**
— the DSP stays compiled in and keeps running, so a song saved using the mid EQ
bands or the Gristleizer plays back exactly as saved with its menus switched off.
Sear's setting is `enableSear` on both lines, so that one carries across a firmware
swap.

`osc/type.h` needed more than a gate on both lines: its option list used to hide DX7
and Plaits behind one predicate with a fixed ±2 index shift. Plaits can be switched
off on its own now, so the shift is gone and both the list and the conversion come
from one `offeredTypes()`. One deliberate 1.2.1 difference: with no mic or line in,
the collapsed INPUT entry still selects `INPUT_L` (where the old arithmetic landed)
rather than 1.3's `INPUT_STEREO` — a behaviour change doesn't belong in a port.

### One 1.2.1 branch is still unmerged

**`feat/gristle-on-12`** — a master switch so the Gristleizer can be bypassed.
Without it there is no switch to wrap, so the toggle hides the whole GRISTLE
submenu. A single commit off Chopin.

(`feat/eq-readout-12` — the Hz/dB band readout — went in at `f4b73ca4` on
2026-08-22 and is on the 1.2.1 tip.)

---

## Reference

| Document | What it holds |
|---|---|
| `CHROMA-MANUAL.md` | Operating manual for the Harmonic layout and Brush, both lines |
| `CLOUDS-STATUS.md` / `CLOUDS-STATUS-12.md` | Clouds: what works, what doesn't |
| `CLOUDS-DESIGN.md` | Clouds memory and effect-chain design |
| `KIT-SPLIT-FEASIBILITY.md` | Why Kit Split is built the way it is |
| `KIT-MERGE-DESIGN.md` | Undo, and the merge direction |
| `DSP-PORTING-FEASIBILITY.md` | What can and can't be carried onto this hardware |
| `RUDEMENT-FEATURES.pdf`, `manuals/` | The feature manuals, as shipped |
| `docs/DEVELOPMENT-NOTES.md` | Toolchain traps, the FUSE mount problem, CRLF. **Tracked on `beta-1.3`, not checked out at the root** — read it with `git show beta-1.3:docs/DEVELOPMENT-NOTES.md` |

`HANDOFF.md` — the old branch map — is no longer at the root. The last copy is in
`_to_delete/wt/HANDOFF.md`; rescue it before deleting that folder if you want it.
It was written at the 1.2.1/1.3 split and predates Kit Split, AUX Sends and Chroma,
so the branch table in it is stale either way.
