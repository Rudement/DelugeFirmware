## Hardware result 2026-08-09: passes

Verified on `deluge-v1_2_1-rudement+2026_08_08-4d68a0b1.bin` and
`deluge-v1_3_0-rudement+2026_08_08-a99b641b.bin`.

The readout is correct, Treble included — which was the one most likely to look like a bug,
since its dB figure tracks the Freq knob where the bell bands deliberately do not.

That also confirms the point of moving the preset bases and octave multipliers into
`dsp/eq_bands.hpp`: the menu reads the same coefficients the filter runs with, so readout and
filter cannot drift apart.

Closing.
