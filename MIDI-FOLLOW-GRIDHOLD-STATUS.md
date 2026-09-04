# MIDI Follow: hold a grid pad to lock the Follow target — confirmed working 2026-09-04

Branch: **`feat/aux-sends-13-gated`**, on top of `c51d8254`. Four files changed,
uncommitted in the working tree (consolidated patch below covers all of it).

**Status: built, flashed, and confirmed working on hardware.** Lock, unlock, the pulse
staying pinned to the locked pad, and the on-screen lock/unlock text have all been tested
and confirmed. Not yet committed to git.

## The problem this solves

Grid (Launch/green) mode and MIDI Follow's A/B/C channels fight over the same touch.
Tweaking a synth track with MIDI Follow, then tapping a drum pad to trigger a clip, made
Follow instantly retarget to the drum track — killing the synth tweak in progress. No
channel is free to route around it (channel 1 is the only spare channel on this rig, and
one page/scene is nowhere near enough to cover the whole setup).

## The fix

A grid pad tap in Launch mode still launches/stops the clip exactly as before. Holding
the pad past **Settings > Defaults > Hold Time** — the same timer the Shift button uses,
reused on purpose for muscle memory — claims that clip as the MIDI Follow target for the
A/B/C channels. A popup confirms it: "Follow: `<track name>`". The target sticks until a
different pad is held the same way, or the *same* pad is held again to release the lock
("Follow: unlocked"). Ordinary taps never touch it. Scoped to Launch/green mode only
(Edit/blue mode untouched — not used on this rig).

The grid's selection pulse (the light that shows which pad is "selected") now follows the
lock too: while nothing's held, it stays pinned on the locked pad instead of jumping to
whatever was last tapped. It still shows live touch feedback while a pad is actively held.

Three code paths, because the lock has to survive every way a CC or a redraw can arrive:
- `MidiFollow::getGridSelectedClip()` catches a knob turned *during* a hold, once Hold Time
  has elapsed.
- `session_view.cpp`'s `gridHandlePadsLaunchWithSelection()` release handler catches a hold
  released with no CC having arrived during it — this is also where the lock/unlock and its
  popup are decided.
- `session_view.cpp`'s `gridPulseSelectedClip()` — a *separate*, timer-driven function that
  redraws the pulse roughly every tick regardless of press state — is what actually needed
  to know about the lock; a one-shot fix elsewhere kept getting overwritten by it within a
  fraction of a second.

## Also fixed along the way

**Song-load freeze.** `deleteOldSongBeforeLoadingNew()` (the ordinary "Load Song" path,
taken whenever playback isn't active — most of the time) destructs every Clip in the old
Song without ever telling MIDI Follow, unlike the separate `preLoadedSong` swap path used
while playback *is* active. That left `gridSelectedClip` (and the pre-existing
`clipForLastNoteReceived`) dangling into freed memory. Fixed by adding the same
`midiFollow.clearStoredClips()` call to `deleteOldSongBeforeLoadingNew()` in `deluge.cpp`.
Confirmed this stopped the freeze that showed up during testing.

**A "MIDI Follow went silent" scare turned out to be unrelated to any of this** — traced
through Follow channel settings, MIDI Takeover, and Feedback Channel before landing on: an
older flash of the exact same commit worked fine, so it was very likely a flashing hiccup
during one specific test, not a code regression. Worth knowing for next time: if Follow
ever goes silent again, check Settings > MIDI > Takeover (should be Jump) and MIDI Follow >
Feedback > Channel (a feedback channel matching the input channel can self-interfere on a
merged/shared bus) before assuming it's this feature's fault.

## Files

- **`07-midifollow-gridhold-full-13.patch`** — consolidated patch covering everything above
  (grid-hold-to-select, the freeze fix, unlock toggle, pulse pin, lock/unlock popup) against
  `c51d8254`. Supersedes `05` and `06`, kept those as well for the individual-step record.
- **`run-build-gridhold-13.cmd`** — build script, same convention as `run-build-aux-13.cmd`.
  Asserts the branch, does *not* wipe the build directory, builds, copies the newest `.bin`
  with a `GRIDHOLD-` prefix, tails the log.
- **`run-build-baseline-check.cmd`** — builds a clean, zero-patch baseline for isolating
  regressions. Not needed for normal use; kept for next time something looks broken and it's
  unclear whether this feature is the cause.

## Not done here

- **`04-midifollow-defaults-13.patch`** is unrelated and still parked — a
  `readDefaultsFromFile()` hygiene fix for `MIDIFollow.XML` being read as exhaustive rather
  than additive. Not built, not requested.
- Nothing here is committed to git yet.
