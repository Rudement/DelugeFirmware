## EQ: report each band in Hz and dB

Backport of `a3c21ebc` from the 1.3 line, reimplemented on this branch's menu API.

Preset bases and octave multipliers move out of `processFX()` into `dsp/eq_bands.hpp`, so the
menu readout is computed from the same coefficients the filter runs with rather than a second
copy that could drift. Values unchanged.

**Base:** `a2e333b9` (Chopin 8.8.26, 1.2.1 line). Independent — no other feature branch underneath it.

### Why the readout was reimplemented rather than ported

1.3 hangs this off `getNotificationValue(StringBuf&)`. This branch has no `StringBuf`, no
`getColumnLabel`, no `menu_item/eq/` hierarchy — bringing them over would mean reworking the
core menu system on a release branch. `drawInteger()` / `drawValue()` are both already virtual,
so `EqFreqParam` and `EqGainParam` override those and format into a plain char buffer.

Same numbers, same rounding, same shapes — `466Hz`, `4.87kHz`, `+12.0dB`, `OFF` — via integer
arithmetic, since float printf is not worth pulling in for four menu items.

7SEG drops the unit and the `+`: four characters, and `-23.9` uses all of them.

### Deliberately not ported

The `globalParamToCC` fix from the original commit. That bug is a consequence of 1.3's split
forward/reverse CC maps. This branch uses the single `paramToCC[x][y]` grid read in both
directions, so the mismatch cannot occur here.

### Not yet done

- [ ] **Untested on hardware.** Check Treble specifically — see linked issue.
