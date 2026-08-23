# Chroma — Harmonic Layout & Chord Tools

A chord explorer beside an isomorphic visualiser, a brush that stamps chords into
the piano roll, and the smaller tools that come with them. Documented as the code
behaves, on both firmware lines — where the original author's docs and the code
disagree, the code wins and it says so.

Branches, build numbers and porting notes are in **`HOW-TO-APPLY.md`**. This is
the playing manual.

---

## 1. What's in it

| Feature | Where | Toggle |
|---|---|---|
| **Harmonic layout** — chord palette + isomorphic visualiser | Keyboard View | none |
| **Harmonic Brush** — stamp a chord into the piano roll | any keyboard layout → piano roll | `Chord Brush (BRSH)`, **On** |
| **Chord inspector** — names any chord you hold | Keyboard View and the piano roll | none |
| **Note preview** — playback lights the keyboard grid | all keyboard layouts | `Keyboard Note Preview (PREV)`, **On**, no menu item |
| **Retrospective capture** — SHIFT+RECORD dumps what you noodled | Keyboard View | `Retrospective Capture (RCAP)`, **On** |
| **Host sysex** — chord state out to a companion app | MIDI | none; dormant until a host handshakes |

Toggles live in `SETTINGS → COMMUNITY FEATURES` and persist to
`SETTINGS/CommunityFeatures.XML`.

---

## 2. Getting in

**Hold KEYBOARD and turn the SELECT encoder** to cycle layouts. Harmonic is last in
the list, so it's one step back from Isomorphic turning anticlockwise. On OLED it
names itself `Harmonic`; on a 7-seg it's `HARM` on 1.3 and scrolls `HARMONIC` on
1.2.1.

Two hard requirements, enforced in code: **scale mode must be on** — the whole
layout is built from scale degrees — and it is **melodic only**, so it won't open on
a kit.

On a scale with fewer than seven notes you get fewer palette columns and the
leftovers go black. The Calculator needs exactly seven and stays dark otherwise.

### Your first minute

1. **Press column 0, row 2** — the bottom-left area, third row up. That's the tonic
   triad. It sounds, and the display names it.
2. **Look right.** The same chord is lit on the iso panel in the column's own
   colour, with an amber pad an octave below it — that's the bass note.
3. **Look left.** Two or three columns are pulsing white on rows 2–5. That's the
   Calculator telling you where this chord usually goes. Press one.
4. **Hold a chord pad and turn the vertical encoder.** The voicing walks while the
   chord rings. Let go of the pad and it springs back.
5. **Hold LEARN and tap anything.** The surface goes silent and every pad tells you
   what it is.

---

## 3. The grid

Sixteen columns: two seven-wide grids with two control columns wedged between them.

```
 x:   0    1    2    3    4    5    6     7      8     9   10   11   12   13   14   15
    ┌──────────────────────────────────┬──────┬──────┬──────────────────────────────────┐
    │        PALETTE (chords)          │ PAL  │ ISO  │       ISO (play surface)         │
    │     7 scale-degree columns       │ CTL  │ CTL  │    isomorphic note grid          │
    └──────────────────────────────────┴──────┴──────┴──────────────────────────────────┘
                                       crimson purple
```

**SWAP** (palette control, row 6) mirrors the whole thing — palette right, iso left —
and each control column moves with the grid it drives.

Rows are numbered **0 at the bottom, 7 at the top**, as the hardware is. Every table
below reads top-down, the way the column sits under your hand.

---

## 4. The palette — the chord explorer

**Columns are the seven scale degrees**, in order, each a hue chosen so neighbours
never blend:

| Column | 0 | 1 | 2 | 3 | 4 | 5 | 6 |
|---|---|---|---|---|---|---|---|
| Degree | 1 | 2 | 3 | 4 | 5 | 6 | 7 |
| Colour | red | cyan | yellow | blue | green | magenta | orange |

Whether a degree reads as major, minor or diminished is computed from the current
scale, not fixed — the Roman numeral on the display carries the case and the `o`/`+`.

**Rows are richness**, stacking upward, each adding another third:

| Row | Chord | Notes | Suffix shown |
|---|---|---|---|
| 7 | 13th | root 3 5 7 9 11 13 | `13` |
| 6 | 11th | root 3 5 7 9 11 | `11` |
| 5 | 9th | root 3 5 7 9 | `9` |
| 4 | 7th | root 3 5 7 | `7` |
| 3 | 6th | triad + a **major** 6th, borrowed if the scale only has a ♭6 | `6` |
| 2 | **Triad** | root 3 5 | — |
| 1 | 5th | root + 5th | `5` |
| 0 | **Root** | single note — the bass / anchor lane | — |

Brightness fades as you climb, 255 at the bottom to 42 at the top, so the column
reads as getting denser. A picked cell blows out to a near-white tint of its own hue
so it pops even on the dim upper rows.

`sus2` and `sus4` are deliberately not on this ladder — they're alterations, made in
EDIT mode by toggling the 3rd out and the 2nd or 4th in.

**Press a pad** and it sounds the full voicing, lights its shape on the iso panel,
and asks the Calculator where you might go next. It also names itself — see below,
because the name is less than it looks.

### What the display actually says

The palette's name is **absolute then Roman**, and the absolute half is only the root
plus the ladder suffix. There is no chord quality in it. On a I7 in C you get:

```
C7  I7
```

Not `Cmaj7` — the layout never writes a quality. A ii triad reads `D  ii`, and a
tonic triad reads `C  I`. The Roman numeral is where the quality lives.

Worth knowing: on any row above the root, the chord inspector (§12) usually
overwrites this within the same pad scan, so what settles on screen is often the
inspector's reading rather than the palette's. They agree about the chord; they
disagree about the spelling and the order.

### The Calculator

Palette control row 7, **on by default**. It ranks the top three next chords and
**pulses them in white** on rows 2–5 of their columns — white over the hue, not the
hue brightened. The chord you're on stays at full brightness so you can see where
you are. It needs a seven-note scale; on anything else it stays dark.

---

## 5. The iso panel — the play surface

A normal isomorphic keyboard, tinted to the **key's own mood colour** — each of the
twelve keys has one, C a warm coral, F♯ teal, B bright blue. It's anchored one
octave below the palette's register so the chord you pick lands mid-panel.

**The vertical encoder scrolls it**, clamped to octaves 1–8, whenever no chord pad is
held. The palette's own octave is separate — that's the crimson OCT pads. Setting
them apart is the point: voice chords low, play the melody high.

The colour language is strict, and it is the layout:

| What you see | What it means |
|---|---|
| **Chord colour, full bright** | a note of the current voicing, at its primary (lowest) position |
| **Chord colour, half bright** | that voiced note repeating higher up the grid |
| **Amber / gold** | the **bass spotlight** — one octave below the voicing's lowest note. Sits at its true register, so it may be below your current scroll |
| **Key colour, bright** | a note sounding right now |
| **Key colour, mid** | the tonic, and any shape held in the chord bank |
| **Key colour, faint** | the scale backdrop |
| **Dark** | off-scale, in chromatic view only |
| **White wash** | a pad under your finger this instant |

One catch the table can't show: **while a chord is sounding, every occurrence of it
goes full bright.** The primary/repeat distinction appears when the notes stop — so
it teaches you the shape after you play it, not during.

**Free play on the iso clears the chord selection**, unless STICKY is on or the
octave picker is open.

---

## 6. Control columns

The names in the Control column are **exactly what LEARN says** when you tap that
pad, so you can check this document against the device.

### Palette control (crimson) — shapes the chord

| Row | Control | Behaviour | Display |
|---|---|---|---|
| 7 | **CALCULATOR** | next-chord suggestions on/off | `CALC` / `OFF` |
| 6 | **SWAP SIDES** | mirror palette and iso | `SWAP` / `NORM` |
| 5 | **PALETTE OCTAVE UP** | chord register, 1–8 | `OCT5` |
| 4 | **PALETTE OCTAVE DOWN** | " | `OCT3` |
| 3 | **OCTAVE STACK PICKER** | bottom iso row becomes octave toggles | `PICK` / `STAK` |
| 2 | **SPREAD (DROP-ROOT)** | cycles 0–3. Silent until you replay the chord | `SPR2` |
| 1 | **INVERSION** | cycles root→1st→2nd→3rd, re-strikes so you hear it | `INV1` |
| 0 | **CLEAR** | clears the selection — or toggles VOICED/BARE while PROG is held | `CLR` / `VOIC` / `BARE` |

### Iso control (purple) — shapes the surface

| Row | Control | Behaviour | Display |
|---|---|---|---|
| 7 | **VIEW: IN-KEY / CHROMATIC** | in-key ↔ chromatic isomorphic | `KEY` / `CHRO` |
| 6 | **SHOW CHORD SHAPE** | light the voicing, or a clean grid | `SHOW` / `HIDE` |
| 5 | **STICKY (HOLD CHORD)** | keep the chord through free play | `HOLD` / `FREE` |
| 4 | **OVERLAY: ONE / LATTICE / DIFF** | cycles the three overlays | `LATT` / `DIFF` / `ONE` |
| 3 | **SNAP ISO TO CHORD** | on pick, jump the iso to the chord's octave | `SNAP` / `STAY` |
| 2 | **PROGRESSION (HOLD + DIAL)** | momentary — hold and dial, see §8 | preset name |
| 1 | **EDIT NOTES** | iso taps toggle notes in and out of the voicing | `EDIT` / `PLAY` |
| 0 | **AUDITION (HOLD TO HEAR)** | momentary — hold to hear the voicing | — |

No clear pad on the purple column; row 2 is PROG. The column is banded by shade —
rows 7–3 are the *seeing* controls, 2–1 *shaping*, 0 *hearing*.

### The three overlays

- **ONE** — the voicing you picked, one shape.
- **LATTICE** — every position of every chord tone across the grid, glowing faintly
  and fading upward, chord tones brighter than extensions. Shows you everywhere the
  chord is available to play.
- **DIFF** — voice leading. Notes **held** from the previous chord sit steady, notes
  **arriving** breathe bright, notes **leaving** ghost in the *previous* chord's
  colour, breathing in antiphase. Watch a ii–V–I and you see the voices walk.

### SNAP vs STAY

A performance decision. SNAP (default) keeps the iso locked to the chord's register
so the shape is always visible. STAY parks it where you scrolled it — chords low in
the left hand, melody high in the right.

### EDIT NOTES

Forces SHOW and STICKY on, stops single taps sounding, and makes each iso pad toggle
that note in or out of the voicing. **Every edit that lands on a chord the table
recognises re-names it** — sculpt something it doesn't know and the display simply
keeps the last name, which reads as frozen. Nothing sounds while you sculpt; tap
AUDITION to hear it.

---

## 7. The voicing engine

```
base chord → INVERSION → SPREAD → VOICING WALK → OCTAVE STACK → what you hear
```

- **INVERSION** (0–3) rotates: the lowest N notes go up an octave and the chord
  re-sorts.
- **SPREAD** (0–3) drops the lowest N notes an octave — drop-root openings.
- **VOICING WALK** (−24…+24) walks the whole voicing a note at a time: each step up
  lifts the current lowest note an octave, each step down drops the current highest.
- **OCTAVE STACK** copies the chord into any of seven octaves (−3…+3, base always
  on), deduplicated and clamped.

### The spring-loaded dial

The part worth practising.

| Gesture | Result |
|---|---|
| Hold a chord pad, turn the vertical encoder | the voicing walks while the chord rings — `V3`, `V-2` |
| Release the chord | springs back to home |
| Click the vertical encoder | springs back to home now — `VOI0` |
| Hold the encoder in and turn | dials *where* home is — `H4` |
| Hold a chord pad, turn the horizontal encoder | blooms the octave stack outward: −1, +1, −2, +2, −3, +3 — `STK2` |

### The octave picker

Palette control row 3 (`PICK`) turns the **bottom row of the iso** into seven octave
toggles: local columns 0–6 are octaves −3 to +3, column 3 is the base and is always
on. Toggling re-strikes the chord so you hear it. Press again (`STAK`) and the row
goes back to playing notes.

---

## 8. Progressions

**Hold iso control row 2 (PROG)** and the encoders take over:

- **Vertical** — browse which progression. Resets you to step 1.
- **Horizontal** — walk its chords, wrapping, so a vamp loops.
- **Holding sustains** the current step. Release and the chord stays loaded.
- **CLEAR while PROG is held** toggles `VOIC` / `BARE`.

**Every step change reloads the voicing.** Whatever SPREAD, INVERSION and STACK you
had dialled in is overwritten each time you turn the horizontal encoder — in both
VOIC and BARE. All five built-ins bake a neutral voicing, so on those two the
practical difference is nil; the distinction only bites with chord packs that carry
one.

Built-ins, all loaded as **7th chords**, not triads:

| Name | Degrees |
|---|---|
| `251` | ii – V – I |
| `1564` | I – V – vi – IV |
| `EPIC` | i – VI – III – VII |
| `ANDL` | Andalusian: i – VII – VI – V |
| `DORI` | Dorian vamp: i – IV |

They're scale-relative, so they resolve into whatever key and scale the song is in.

### Chord packs

Drop `.chordpack` files in a **`CHORDS/`** folder at the card root and their
progressions appear after the built-ins. **Scanned lazily on first PROG use, never at
boot** — a malformed pack can't brick the device, it just doesn't load.

```xml
<chordpack>
  <progression>
    <name>NEO</name>
    <step> <degree>1</degree> <rich>9</rich>  <octdn>1</octdn> <spread>1</spread> </step>
    <step> <degree>6</degree> <rich>7</rich>  <inv>1</inv> </step>
    <step> <degree>4</degree> <rich>triad</rich> <octup>1</octup> </step>
    <step> <degree>5</degree> <rich>13</rich> <spread>2</spread> <inv>2</inv> </step>
  </progression>
</chordpack>
```

- `degree` — **1-based**, clamped 1–7.
- `rich` — `root` or `1`, `5`, `triad`, `6`, `7`, `9`, `11`, `13`. Anything else
  becomes `7`.
- `octup` / `octdn` — 0–3 octave layers above / below the base.
- `spread`, `inv` — 0–3 each.

Limits: **16 pack files**, **48 progressions** including the built-ins, **8 steps**
each, **7-character** names. Unknown tags are skipped, so your own metadata is safe.
A progression with no `<name>` is auto-named `PACK`; one that yields no valid steps
is dropped silently.

---

## 9. LEARN — the inspect mode

**Hold LEARN and the entire surface goes silent.** Every pad you tap names itself and
does nothing else — no notes, no state changes, no toggles fired.

- A **palette pad** names its chord, Roman first: `I7  C7`.
- An **iso pad** names its note — always spelled with sharps, `A#3`, even in a flat
  key where the palette says `Bb`.
- A **control pad** names its function in full: `OVERLAY: ONE / LATTICE / DIFF`.
- A **sidebar chord-bank pad** reports what's stored (`BANK`) or `EMPTY`.
- A **sidebar velocity or mod pad** names its column: `VELOCITY`, `MOD`.

Names fire once per press, not every frame. Release LEARN and nothing you touched has
changed.

---

## 10. The Harmonic Brush

Capture a chord anywhere, stamp it into the piano roll like a note.

The reason it works from everywhere: it captures **the notes currently sounding**,
not a chord index. The Harmonic palette, Chord, Chord Library, or four notes you're
just holding down on the Piano layout — all the same to it.

**The two steps that matter:**

**Arm** — in a keyboard view, hold the chord and press the **SELECT encoder**. 7-seg
shows `PEND`; OLED says *Chord armed*. The brush survives the switch to the piano
roll.

**Place** — press **KEYBOARD** to get to the piano roll, then tap a step. The chord
auditions the moment you press, and is written on release. `PLCD` / *Chord placed*.
The chord keeps its own pitches; only the column matters.

**And then, optionally:**

- **Length** — hold the start step and press any other column. The chord spans from
  one to the other, in either direction, and the last press wins so you can extend or
  shorten before letting go.
- **Re-voice by ear** — while holding the placement pad, turn the **SELECT encoder**.
  Clockwise lifts the lowest note an octave, anticlockwise drops the highest, and it
  re-auditions as you turn — `VUP` / `VDN`. What you hear is what gets written.
- **Stamp again** — it stays armed, so you can place the same chord across several
  steps.
- **Clear** — press the SELECT encoder while armed but not holding notes. `CLR`.

**It disarms** when you switch layout, when you click SELECT, or when you leave the
piano roll for any other screen — which includes going back to Keyboard View, so arm
it *there* and come *here*.

Each stamped chord is one undo step. Melodic clips only.

---

## 11. Retrospective capture

Every note you play in Keyboard View is buffered in the background, **even when
you're not recording**. **SHIFT + RECORD** dumps the buffer into the current clip.

- **128 notes.** When it fills it keeps the earliest take rather than overwriting.
- Timing is kept in audio samples and converted at dump time, so **it works with the
  transport stopped**.
- The clip **grows to fit**, rounded to whole bars: two bars noodled over a one-bar
  loop becomes a real two-bar clip instead of folding back on itself.
- The display shows how many notes landed. `NONE` means the buffer was empty,
  `MELODIC ONLY` means you're on a kit.
- The buffer clears when you leave Keyboard View, switch layout or change track — so
  it's always "what I just played, here".

---

## 12. The chord inspector

Not part of the Harmonic layout — it works everywhere, and it's easy to trip over
without knowing what it is.

**Hold two or more notes and the display names the chord** instead of the single
note. It matches what you hold against the chord table, spells it to the song's key,
and shows **Roman first, then absolute**:

```
IM7  CM7
```

The table's names are terse — `M`, `-`, `M7`, `-7` — so it reads `CM7`, not `Cmaj7`.
Fewer than two notes, or a set the table doesn't recognise, and you get the ordinary
single-note readout instead.

Two places:

- **Keyboard View**, on any layout, from the notes you're physically holding.
- **The piano roll**, from the audition pads — hold two or more rows. Read-only; it
  never edits notes.

This is also what usually wins the display after a palette pick, which is why the
same chord can be named two different ways a moment apart.

---

## 13. Note preview

With `Keyboard Note Preview` on — it is by default, and it has no menu item —
**sequenced notes light the keyboard grid during playback**. On the Harmonic iso
panel that means your clip plays back as moving shapes on the visualiser: you watch
the progression walk under your fingers.

It lights only the clip you're looking at, and clears on every path out of a note, so
loops, cuts and re-triggers can't leave a pad stuck lit.

---

## 14. Display codes

Everything the layout can put on a 7-seg, in one place. OLED spells most of these out
in words instead.

| Code | Meaning | Code | Meaning |
|---|---|---|---|
| `CALC` / `OFF` | Calculator | `KEY` / `CHRO` | iso view |
| `SWAP` / `NORM` | handedness | `SHOW` / `HIDE` | chord shape |
| `OCT3` | palette octave | `HOLD` / `FREE` | sticky |
| `PICK` / `STAK` | octave picker | `LATT` / `DIFF` / `ONE` | overlay |
| `SPR2` | spread | `SNAP` / `STAY` | iso follows chord |
| `INV1` | inversion | `EDIT` / `PLAY` | edit mode |
| `STK2` | stack layers | `VOIC` / `BARE` | progression voicing |
| `V3` / `V-2` | voicing walk | `PEND` | brush armed |
| `VOI0` | sprung back to home | `PLCD` | chord placed |
| `H4` | voicing home | `VUP` / `VDN` | brush re-voice |
| `CLR` | cleared | `NONE` | capture buffer empty |
| `BANK` / `EMPTY` | LEARN on a bank pad | `MELODIC ONLY` | capture on a kit |

---

## 15. Defaults and limits

**On first entry:** palette octave 3, iso octave 3, in-key view, Calculator on, chord
shown, SNAP on, sticky off, both overlays off, no swap, edit mode off, octave picker
closed, stack = base octave only, spread 0, inversion 0, walk 0, home 0, progressions
VOICED.

**Sidebar:** stays at the factory velocity / mod default. The chord bank is opt-in —
hold a sidebar column and turn the vertical encoder to switch it to CHORD_MEM. The
author removed the auto-default after it surfaced a clip-duplication bug. The `CHORD`
column function is blocked in this layout.

**Things that will surprise you:**

- The **bass spotlight sits below the voicing**, at its true register. If you can't
  see it, scroll down — positional integrity, not a bug.
- **Picking a chord always turns SHOW on**, and turning on an overlay forces SHOW on
  too, so an overlay can't silently do nothing.
- **A PROG step change wipes your live voicing.** See §8.
- **The Calculator is silent on non-7-note scales**, and short scales leave the
  right-hand palette columns black. Neither is broken.

---

## 16. The two firmware lines

The layout, the Brush, the voicing engine, the chord packs and every display code are
the same on both branches — `harmonic.cpp` is byte-identical between them. Three
differences are visible from the front panel:

| | 1.3 | 1.2.1 |
|---|---|---|
| Layout name on a 7-seg | `HARM` | scrolls `HARMONIC` |
| Piano layout in the cycle | present | doesn't exist on this line |
| New l10n strings | 9 | 8 (the layout names itself directly) |

Everything else behaves the same. What each *build* contains does differ — the 1.3
build has Clouds and the 1.2.1 build doesn't — but that predates Chroma.

The seven small API adaptations the 1.2.1 port needed, the branch layout, the build
numbers and what was left out of the original squashed commit are all in
**`HOW-TO-APPLY.md`**.

---

## 17. What has been confirmed on hardware

`integration/chroma-13` at `8b1cbe86`, 22 August 2026. It boots and runs.

| Checked | Result |
|---|---|
| Both toggles present, list scrolls to the bottom | ✓ — the `kNonTopLevelSettings` freeze does not happen |
| Chord packs load from `CHORDS/` | ✓ — and a deliberately malformed pack is ignored without collateral damage |
| Spring-loaded voicing dial | ✓ — holding a chord pad and turning the vertical encoder reports `V1`, `V2`… and re-voices, rather than scrolling the iso |
| LEARN inspect mode | ✓ — surface goes silent, every pad names itself |
| Chord naming (§4) | ✓ — in D with a flat seventh, the triad on the seventh column reads `C  bVII`: absolute first, Roman second, no quality written into the absolute name, and the accidental computed against a major scale rather than stored |

Not yet exercised: the overlays, the Brush, retrospective capture, the chord
inspector, and the whole of the 1.2.1 branch, which has never been flashed.
