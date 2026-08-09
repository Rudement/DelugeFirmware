## Sear: rename from Heat, automatic level matching

Renames the per-voice clipper from Heat to Sear throughout, XML attributes included, and
replaces the static makeup constant with two one-pole followers that track mean `|sample|`
either side of the curve and correct on the ratio.

Full rationale is in the commit message.

**Base:** `a2e333b9` (Chopin 8.8.26, 1.2.1 line). Independent — no other feature branch underneath it.

### 1.2.1-specific deviations

- `kSearDriveBoost` was carried across **by hand**. The +50% drive fix (`a2e333b9`) existed
  only on this line; the 1.3 Sear rename predated it and never saw it. A straight cherry-pick
  would have deleted it silently along with `heat.hpp`.
- 1.2.1's `Voice` is placement-constructed with no `Sound` in reach, so the auto-level seed
  moved from the constructor (where 1.3 puts it) into `noteOn()`, which does have one. Same
  net effect — both run per note-on.
- `fixedpoint.h`: added `subtract_saturation` (no `qsub` helper existed here) and fixed the
  **host** `add_saturation`, which was plain `a + b` and wrapped instead of pinning. Host
  tests previously could not observe a saturation bug.

### Verification

Numerically verified after porting: ceiling 406x, realised ratio 1.42–1.58x, octaves reaching
8, `octaves==0` window narrowing from k<6.25 to k<2.6 — all matching the documented table.

### Not yet done

- [ ] **Untested on hardware.** See the linked issue — `kSearDriveBoost` raises pre-gain into
      the clipper and the auto-leveller then removes the loudness it would have added. Intended,
      but different from what was heard before the port, and nobody has listened since.
- [ ] No read-side XML alias for `heat` → `sear`. Relies on no song ever having been saved
      with the old tags.
