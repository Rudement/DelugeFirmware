## Gristleizer: master switch so the effect can be bypassed (1.3)

Enablement was inferred from the params themselves, which left no way to switch the effect off
without discarding the patch, and was misleading in the other direction — Depth at zero looks
like an off switch and isn't one, because Mode up leaves a static lowpass in circuit.

`UNPATCHED_GRISTLE_ON` replaces that rule. A param rather than a bool so it can be automated,
assigned to a gold knob and driven by **CC 89** — throwing the effect in and out is a
performance gesture, not a setup option. Thresholded at centre, never blended.

While bypassed the SVF state is zeroed, since resuming an automated switch from stale filter
memory ticks. The LFO phase keeps free-running so the chop stays locked to the timeline.

**Base:** `134d000f` → `feat/gristle-13` (`6c02c5d6`) → this.

### Depends on

- **`feat/gristle-13`** — modifies `gristle.hpp`, created there.

### Difference from the 1.2.1 backport

This branch **does** get MIDI Follow CC 89 and touches `midi_follow.cpp`. The 1.2.1 version
cannot — CC 89 is Low Mid Freq there and no free grid position carries a CC.

### Split note

Rebased off the Sear commit; three files had their context reverted to Heat naming. No logic
changed — re-stacking reproduces `chopin-to-13`'s tree exactly (`e203afbc`).

### Not yet done

- [ ] **Untested on hardware.** Bypass behaviour — LFO should stay locked across a toggle.
