# Scene Capture — feasibility

**The function:** you are in grid view with playback running. Some columns are
sounding, each playing a clip from whichever row you launched it from — kick
from row 1, snare from row 3, hats muted. One gesture stamps that exact
combination into a new row underneath the lowest populated one. From then on,
pressing that row's section pad recalls the combination you were listening to.

**Scope as agreed:**

- **Copies, not links.** Capture clones each sounding clip into the new row.
  The captured row is a snapshot — editing the row-1 kick later does not change
  the captured one.
- **Blank where nothing is playing.** A column with no sounding clip gets no
  clip in the new row. Recall simply leaves that track silent.
- **The new row is the first unused section below the lowest populated row** —
  not the bottom of the 8-pad display, and not row 11.
- **The captured row lands muted.** Capture is inaudible: the originals keep
  playing and nothing changes until you press the new row's launch pad.
- **Recall is the existing section pad.** No new launch behaviour.

> Line references were read from the working tree at `feat/clouds-clean-entry`
> @ `0326594a` — the 1.2.1 line, which is what is currently checked out. The
> file paths are the same on the 1.3 tip (`feat/kit-split-13`); the line
> numbers are not. Re-check before cutting code.

**Verdict: feasible, and it is the smallest feature you have added to this
fork.** Every primitive it needs already exists and is already exercised by
shipped code paths. Recall costs you *zero lines* — it is the section pad,
which already does exactly this. The whole feature is the capture step, and
that is one function of roughly forty lines plus a button chord. There is one
place it will bite if you are not deliberate about it — the captured row has to
land muted, with the originals still sounding (§5) — and two hard ceilings you
should decide about up front (§6).

---

## 1. What the grid actually is, in the code

Worth pinning down because the naming does not match the visual.

**Columns are tracks.** `SessionView::gridTrackFromX(x, maxTrack)`
(`gui/views/session_view.cpp:4521`) maps an X position to an `Output` —
your kick track, your snare track. Each `Output` has exactly one active clip at
a time (`Output::getActiveClip()`, `model/output.h:88`). That single fact is
what makes this feature unambiguous: *there is never a question of which clip a
column is playing*, because a column can only be playing one.

**Rows are sections.** A clip carries a single `uint8_t section`
(`model/clip/clip.h:168`). The mapping is inverted from what you might expect:

```cpp
// gui/views/session_view.cpp:4491
int32_t SessionView::gridSectionFromY(uint32_t y) {
    int32_t result = ((kGridHeight - 1) - y) + currentSong->songGridScrollY;
    if (result >= kMaxNumSections) { return -1; }
    return result;
}
```

Section 0 sits at the **top** of the grid and the numbers increase **downward**.
So "under the lowest populated row" is literally *the highest section number in
use, plus one*. That is a pleasantly cheap thing to compute, and it means the
row appears exactly where you pictured it.

`Section` itself (`model/song/song.h:72`) is almost nothing — a MIDI learn slot
and a repeat count. Membership lives entirely in the clip's `section` byte.
There is no section object to create; a row exists because clips claim it.

## 2. Recall is already built — you write none of it

In grid Launch mode, the left sidebar pad for row Y goes straight here:

```cpp
// gui/views/session_view.cpp:3997
if (on) {
    // Immediate launch if shift pressed
    gridStartSection(section, Buttons::isShiftButtonPressed());
}
```

and `gridStartSection` (`:3679`) either arms the section on the next launch
boundary via `session.armSection()` or, with shift, toggles every clip so that
exactly the clips in that section are playing and everything else is not:

```cpp
if ((clip->section == section && !clip->activeIfNoSolo)
    || (clip->section != section && clip->activeIfNoSolo)) {
    gridToggleClipPlay(clip, instant);
}
```

That is precisely "recall the combination." Quantised launch, immediate launch,
section repeat counts, MIDI-learnable section triggers — all of it comes free
the moment the clips are sitting in that section. **This is why the copies
approach is the right call and not just the cheap one.** A references-based
scene row would have had to reimplement every one of those behaviours.

## 3. Cloning is already built too

`SessionView::gridCreateClip(targetSection, targetOutput, sourceClip)`
(`:3515`) is the exact primitive: it clones a clip, stamps the new section onto
it, and inserts it into the song.

```cpp
newClip = gridCloneClip(sourceClip);      // :3533
...
newClip->section = targetSection;         // :3584
currentSong->sessionClips.insertClipAtIndex(newClip, 0);
```

It is already used for copy-paste between pads (`gridClonePad`, `:3658`). And
because capture always clones **into the same column**, `targetOutput ==
sourceClip->output`, which means the expensive branch — `changeInstrument()`
for a cross-track paste — is skipped entirely. Same-column cloning is the cheap
path through this function.

Audio clips are handled by the same call (`:3628`), so a captured audio column
works without a special case.

## 4. The capture routine

Roughly this, as a new `SessionView::gridCaptureScene()`:

```cpp
// 1. Find the target row: first unused section below the lowest populated one.
//    The sectionUsed[] loop already exists at :1095 — reuse the pattern.
bool sectionUsed[kMaxNumSections] = {false};
for (int32_t i = 0; i < currentSong->sessionClips.getNumElements(); ++i) {
    Clip* c = currentSong->sessionClips.getClipAtIndex(i);
    if (c->section < kMaxNumSections) { sectionUsed[c->section] = true; }
}
int32_t target = 0;
for (int32_t s = kMaxNumSections - 1; s >= 0; --s) {
    if (sectionUsed[s]) { target = s + 1; break; }
}
if (target >= kMaxNumSections) {           // all 12 rows in use
    display->displayPopup(l10n::get(l10n::String::STRING_FOR_NO_FREE_ROW));
    return;
}

// 2. Walk the OUTPUT list, not the clip list — one column, one sounding clip.
Clip* previousCurrent = currentSong->getCurrentClip();
int32_t captured = 0;

for (Output* o = currentSong->firstOutput; o != nullptr; o = o->next) {
    Clip* sounding = o->getActiveClip();
    if (sounding == nullptr || !currentSong->isClipActive(sounding)) {
        continue;                          // muted or stopped column -> stays blank
    }
    // startMuted = true: the clone lands silent, the original keeps playing (§5)
    Clip* made = gridCreateClip(target, o, sounding, /* startMuted = */ true);
    if (made == nullptr) { break; }        // popup already shown by callee
    ++captured;
}

currentSong->setCurrentClip(previousCurrent);
if (captured == 0) {
    display->displayPopup(l10n::get(l10n::String::STRING_FOR_NOTHING_PLAYING));
}
```

Three notes on the loop shape.

**Iterate outputs, not clips.** The obvious version — walk
`currentSong->sessionClips` looking for active clips — breaks, because
`gridCreateClip` calls `sessionClips.insertClipAtIndex(newClip, 0)`. It inserts
at the *front* of the very array you are iterating, shifting every index under
you. Walking the `Output` linked list (`song.h:190`) sidesteps that entirely:
capture adds clips, never outputs, so the list is stable for the whole loop.
It also expresses the feature more directly — one column, one sounding clip, by
construction.

**`getActiveClip()` alone is not "is it playing."** An `Output` keeps its
`activeClip` pointer when the track is stopped or muted, so the instrument holds
its sound and parameters. `getActiveClip()` on a silent column returns the clip
that *would* play. The `isClipActive` check is what makes a muted column come
out blank, which is the behaviour you asked for.

**And it has to be `isClipActive`, not raw `activeIfNoSolo`.**
`Song::isClipActive` (`model/song/song.cpp:4038`) is `soloingInSessionMode ||
(activeIfNoSolo && !getAnyClipsSoloing())` — the soloing-aware answer to "is
this actually sounding right now," which is what your ear is telling you when
you reach for the gesture. Raw `activeIfNoSolo` captures the wrong set the
moment anything is soloed.

## 5. Mute the new row, leave the originals playing

`Clip::cloneFrom` copies the playback state along with everything else:

```cpp
// model/clip/clip.cpp:88
soloingInSessionMode = otherClip->soloingInSessionMode;
armState             = otherClip->armState;
activeIfNoSolo       = otherClip->activeIfNoSolo;
```

So a clone of a *playing* clip comes out of `gridCreateClip` **marked as
playing**. Left alone, capture would put two active clips on the same `Output`,
which is a state the rest of the engine does not expect — one `Output`, one
active clip is an invariant, not a convention (see the `clip != theActiveClip
&& isClipActive(clip)` guard at `model/song/song.cpp:4008`, which exists to
enforce it). Expect stuck notes, a wrong-looking grid, or worse.

**The answer is to mute the captured row on the way in and leave the originals
sounding.** That is the right behaviour on its own merits, not just a fix:
capture should be *inaudible*. You press the gesture, the music does not
change, a new row appears below. Nothing about what you are hearing shifts
until you deliberately press the new row's launch pad.

Two things make this work cleanly:

**The originals keep their track.** `gridCreateClip` only reassigns the
`Output`'s active clip when it created a new track:

```cpp
// gui/views/session_view.cpp:3648
if (targetOutput == nullptr && !newClip->output->getActiveClip()) {
    newClip->output->setActiveClip(modelStack);
}
```

Capture always passes a real `targetOutput`, so `Output::activeClip` stays
pointed at the clip that is already playing. The originals carry on untouched
with no work from us.

**But mute it inside the clone, not after.** `resyncNewClip` (`:1731`) is
gated on exactly the flag we are clearing:

```cpp
if (session.hasPlaybackActive() && playbackHandler.isEitherClockActive()
    && currentSong->isClipActive(newClip)) {
    session.reSyncClip(modelStackWithTimelineCounter, true);
}
```

and `gridCreateClip` calls it internally at `:3646`, *before* it returns the
pointer to you. Muting after the call therefore leaves a window in which the
clone is resynced onto the playhead as a live clip on an already-occupied
track. Better to give `gridCreateClip` a defaulted parameter:

```cpp
Clip* gridCreateClip(uint32_t targetSection, Output* targetOutput,
                     Clip* sourceClip, bool startMuted = false);
```

and clear the three flags right after `gridCloneClip` returns (around `:3536`),
before the section assignment and the resync. Defaulted, so no existing caller
changes behaviour. There is in-tree precedent for the reset itself —
`setupForRecordingAsAutoOverdub` (`clip.cpp:124`) does the same thing right
after copying basics.

**Worth checking on hardware before you write anything:** does the *existing*
copy-paste path have this same problem? Clone a currently-playing clip to an
empty pad with the two-pad gesture in grid Edit mode and see whether the grid
shows two lit clips in that column. If it does, that is a pre-existing bug in
`gridClonePad`, and `startMuted` is very likely the fix there too.

## 6. The two ceilings — decide these before you build

**The row count, total - and it is not the same on both lines.**
`kMaxNumSections` is **12** on 1.2.1 (`definitions_cxx.hpp:490`) and **24** on
1.3 (`definitions_cxx.hpp:483`); upstream raised it. That is the hard ceiling on
captures, shared with whatever rows your song already uses. A song with six
populated rows gets six captures on 1.2.1 and eighteen on 1.3, then the popup.
It is a firmware-wide constant with save-file and MIDI-learn implications;
raising it on the 1.2.1 line is a much bigger job than this feature and should
not ride along with it.

*(Corrected 2026-09-01: this section originally said twelve for both lines.)*

**RAM.** Every capture clones every sounding clip. A six-track song captured
four times is twenty-four extra clips carrying full note data. `gridCreateClip`
already fails gracefully — it surfaces `Error::INSUFFICIENT_RAM` and returns
`nullptr` (`:3591`) — so the failure mode is a popup and a partially built row,
not a crash. Worth deciding: on a partial failure, do you leave the half-built
row, or unwind it? Leaving it is less code and arguably more honest; unwinding
is friendlier. I would leave it and say so in the manual.

## 7. The gesture

`SAVE` is already a modifier in session view — `UI_MODE_HOLDING_SAVE_BUTTON`
is live (`:388`) and `SAVE` + a held grid pad deletes a clip (`:454`). Nothing
claims **SAVE + a left-sidebar section pad**, and it reads correctly: *save
this combination.*

Candidates, with what each one costs you:

| Gesture | Collision |
|---|---|
| **SAVE + any section pad** | None found. Reads as "save the combination." Recommended. |
| SHIFT + section pad | **Taken** — that is immediate (unquantised) section launch, `:3997`. |
| Hold section pad in Launch mode | **Taken** — press arms immediately, so there is no free hold. |
| Hold section pad in Edit mode | Taken by the repeat-count editor (`beginEditingSectionRepeatsNum`, `:3807`), though it is meaningless on an empty row and could be special-cased. Fiddly. |
| A fifth sidebar mode pad | Free, but the right-hand mode column is already four pads deep and this is not a mode. |

With SAVE + section pad, the pad you press does not need to be the target row —
capture computes its own target. That is a small mercy, because the target row
may be scrolled off-screen.

## 8. Toggle and strings

Per the usual fork convention, this wants its own Community Features entry.
Add `SceneCapture` to `RuntimeFeatureSettingType`
(`model/settings/runtime_feature_settings.h:69` area), a `SetupOnOffSetting`
alongside the others in `runtime_feature_settings.cpp`, and a `SettingToggle`
in `gui/menu_item/runtime_feature/settings.cpp:95`.

**Do not call it `RetrospectiveCapture`** — that name is taken, by the keyboard
note-buffer feature (`keyboard_screen.cpp:217`). "Scene Capture" is clear and
does not collide.

Strings needed: the menu name, plus `STRING_FOR_NO_FREE_ROW` and
`STRING_FOR_NOTHING_PLAYING`. The 7-segment is four characters
(`kNumericDisplayLength = 4`, `definitions_cxx.hpp:486`), so `SCENE` is out on
length alone before the font even gets a say. `FULL` and `NONE` are safe
candidates for the two popups; `CAPT` or `SCEN` for the menu entry — check them
against the 7-seg font the same way `WISP` got caught and became `DUST`.

## 9. Suggested order

1. **Hardware check first (ten minutes, no code).** Clone a playing clip with
   the existing copy-paste gesture and watch the column. That answers §5 and
   tells you whether you are writing a fix or just a feature.
2. **`gridCaptureScene()` behind the toggle**, wired to SAVE + section pad,
   with the two popups. This is the whole feature.
3. **Confirm on hardware**: capture while playing, capture with a track soloed,
   capture with an audio track sounding, capture until the 12-row ceiling,
   then save the song, reload it, and recall each captured row. Save/load needs
   no work — captured clips are ordinary clips in an ordinary section, so the
   existing file format carries them — but it is exactly the kind of claim that
   deserves a reload before you believe it.
4. **Manual entry** in `manuals/Rudement-Features-Manual.html`, with a grid
   figure showing the captured row appearing beneath the populated ones.
5. **Backport to 1.2.1** as `feat/scene-capture-12` off `base-12`, with the 1.3
   work on `feat/scene-capture-13`.

## 10. What this is not

Naming it honestly so the manual entry does not overpromise: this is **Ableton's
"Capture and Insert Scene"**, minus the live link. It captures what you are
hearing. It does not follow later edits to the source clips, it cannot capture
more times than your line has rows (twelve on 1.2.1, twenty-four on 1.3), and it costs RAM per capture. Within those bounds it
does the thing you described, and it does it with about forty lines of new code
sitting on top of machinery that is already load-bearing elsewhere in the
firmware.
