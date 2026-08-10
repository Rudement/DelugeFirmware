## Hardware result 2026-08-09: still too tame

Flashed `deluge-v1_2_1-rudement+2026_08_08-4d68a0b1.bin`. `kSearDriveBoost` did **not** fix it.

The numbers were already confirmed — ceiling 406x, realised ratio 1.42–1.58x, octaves reaching
8 — so this is not a case of the boost failing to apply. It applies and it still sounds tame.

## Why more pre-gain probably will not help

The auto-leveller's stated design goal is that "only the amount the clipper is ADDING is
corrected". Two one-pole followers track mean `|sample|` either side of the curve and the
corrective gain is their ratio. **By construction, every bit of loudness the extra drive
produces is divided back out.**

That is worth sitting with, because the version *before* the leveller was reported from
hardware as "super hot, I have to turn the volume down past 15". It read as drive partly
because it got louder. Loudness is a large component of perceived saturation, and
level-matching removes that cue completely.

So raising `kSearDriveBoost` again adds harmonics while the leveller keeps cancelling the
loudness. Expect diminishing returns.

Note also that the alternative documented in `sear.hpp` — scaling `level` by 1.073 instead of
adding the constant — is the **wrong direction** for this symptom. It gives mid-knob positions
*less* than the full 1.5x.

## Three hypotheses

1. **Insufficient pre-gain.** The simple reading. Raise `kSearDriveBoost`.
2. **The leveller is working too well.** Level-matched saturation reads as tame. The fix would
   be to let some loudness through — a partial correction rather than a full one.
3. **The curve.** `softClipCubic` is gentle. Past a point, more gain into a cubic soft clip
   asymptotes to something smooth rather than getting nastier. If so this is a waveshaper
   question, not a gain question.

## The experiment that discriminates

**Force the corrective gain to unity and listen.**

- Sounds right immediately → hypothesis 2. No amount of constant-bumping fixes it; the
  leveller needs to correct partially, not fully.
- Still tame with the leveller out of the picture → hypothesis 3, the curve.
- Audibly louder but not dirtier → hypothesis 1, and the boost is genuinely too small.

This is minutes of work and it rules out two of three. Worth doing before touching any
constant.

## Constraint to respect either way

Whatever changes, the thing that must not regress is the reason the leveller exists: no single
constant works, because the required correction spans nearly 14 dB across input levels at one
knob position. Going back to a fixed makeup value reintroduces the fault that produced both
"ducked instead of dirtying" and "super hot".

Any partial-correction scheme needs to stay a **ratio** tracker and must not become a
compressor — note dynamics have to keep passing through untouched.
