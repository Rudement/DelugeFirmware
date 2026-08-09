## Gristleizer: master switch so the effect can be bypassed

Enablement was previously inferred from the params themselves. That left no way to switch the
effect off without discarding the patch, since every one of the nine knobs has a musically
valid setting that is not its default — and was misleading in the other direction, because
Depth at zero looks like an off switch and isn't one.

`UNPATCHED_GRISTLE_ON` replaces that rule. It is a param rather than a bool so it can be
automated and assigned to a gold knob. Thresholded at centre, never blended, so an automation
ramp flips it once at the halfway point instead of fading the effect in.

Legacy songs carry the nine param tags and no `gristleOn`; `inferLegacyGristleOn()` reapplies
the old rule as each gating param is read. An explicit `gristleOn` tag wins from either side.

**Base:** `a2e333b9` (Chopin 8.8.26, 1.2.1 line). Independent — no other feature branch underneath it.

### 1.2.1-specific deviations

- **No MIDI Follow CC and no grid shortcut.** 1.3 gives On CC 89; here 89 is already Low Mid
  Freq. On this branch a CC is inseparable from a grid position, and a sweep of all three
  shortcut tables against `defaultParamToCCMapping` found **zero** positions carrying a CC and
  holding no param. Giving On a CC would mean evicting a param that has one today. Menu and
  automation reach the switch; MIDI Follow does not. *(An earlier attempt silently stole
  Depth's CC 103 — do not repeat that.)*
- `UnpatchedParamSwitch` drops `getNotificationValue()` and `renderInHorizontalMenu()`, and
  uses `Canvas::invertArea()` where 1.3 uses `invertLeftEdgeForMenuHighlighting()`.
- Automation array sizes go 74/39 → 75/40, not 1.3's 97/52 — stock param counts differ. Entry
  counts verified against declared sizes; a mismatch is a hard compile error.

### Split note

This branch was rebased off the Sear commit. Three files needed their context reverted to the
base's Heat naming (`menus.cpp` comment, `automation_view.cpp` param-count comment,
`param.cpp` designated initializer). No logic changed — re-stacking reproduces `chopin`'s tree
exactly (`a54fbe45`).

### Not yet done

- [ ] **Untested on hardware.** Bypass behaviour — LFO should stay locked to the timeline
      across a toggle. See linked issue.
