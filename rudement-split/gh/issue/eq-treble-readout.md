The EQ Hz/dB readout (`b5e2d60d` on 1.2.1, `a3c21ebc` on 1.3) has not been checked on hardware.

### What to check — Treble specifically

Treble's dB figure **tracks the Freq knob**, where the bell bands deliberately do not. That
asymmetry is intended, and it is the thing most likely to look like a bug.

**Above Freq knob 32, Treble should report roughly 0 dB** — the band genuinely stops responding
there. A readout that keeps climbing past that point means the readout and the filter have
drifted apart, which is exactly what moving the coefficients into `dsp/eq_bands.hpp` was
supposed to make impossible.

### Also confirm the shapes render

- OLED: `466Hz`, `4.87kHz`, `+12.0dB`, `OFF`
- 7SEG: same values, no unit, no `+` sign. Four characters — `-23.9` uses all of them, and the
  `.` merges into the preceding digit in `encodeText()`.

All formatted with integer arithmetic, so rounding errors would show as consistently wrong
digits rather than drift.

Builds: `deluge-v1_2_1-rudement+2026_08_08-4d68a0b1.bin`,
`deluge-v1_3_0-rudement+2026_08_08-a99b641b.bin`
