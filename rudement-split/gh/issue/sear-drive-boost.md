Sear was ported from 1.3 to the 1.2.1 line in `def3db2f`, carrying `kSearDriveBoost` across by
hand — the +50% drive fix existed only on chopin and the 1.3 rename never saw it.

`kSearDriveBoost` raises pre-gain **into** the clipper, and the auto-leveller then removes the
loudness it would have added. That is the intent — extra drive lands as saturation rather than
volume — but it is a different result from what was heard on 2026-08-08, and nobody has
listened to it since the port.

### What to check

Does it still sound "too tame"? Sweep the drive knob across its range on material with varied
dynamics.

### If it needs tuning

It is one constant in `sear.hpp`. The documented alternative: scale `level` by 1.073 instead of
adding the offset — same 384x ceiling, removes the bottom-of-knob step, but mid-knob positions
get less than the full 1.5x.

### Verified numerically already

Ceiling 406x, realised ratio 1.42–1.58x, octaves reaching 8, `octaves==0` window narrowing from
k<6.25 to k<2.6. The maths matches the documented table — this issue is only about whether it
sounds right.

Build: `deluge-v1_2_1-rudement+2026_08_08-4d68a0b1.bin`
