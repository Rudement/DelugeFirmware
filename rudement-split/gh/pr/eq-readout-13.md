## EQ: report each band in Hz and dB, and fix the reverse CC map

Preset bases and octave multipliers move out of `processFX()` into `dsp/eq_bands.hpp`, so the
menu readout is computed from the same coefficients the filter runs with rather than a second
copy that could drift. Values unchanged; only their home moved.

Amount controls read out in dB and frequency controls in Hz instead of a bare 0–50.

Also fixes `globalParamToCC` for `BASS`, `TREBLE`, `BASS_FREQ` and `TREBLE_FREQ`, which pointed
at the wrong entries — a consequence of 1.3's split forward/reverse CC maps, where only the
forward half of the EQ block was updated.

**Base:** `134d000f`. No prerequisite branch.

### Split note

One `#include "dsp/gristle.hpp"` was dropped — it came along as stack context from the branch
this was originally written on. Nothing else in the commit references gristle; verified by grep.

### Upstream caveat

This sits on top of two EQ commits that upstream has never seen — `132683c5` (Mid band) and
`134d000f` (four-band, High-Mid bell). Those must land first or this diff will not apply.

### Not yet done

- [ ] **Untested on hardware.** Treble specifically — see linked issue.
