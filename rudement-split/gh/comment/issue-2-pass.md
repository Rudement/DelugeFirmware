## Hardware result 2026-08-09: passes

Verified on `deluge-v1_2_1-rudement+2026_08_08-4d68a0b1.bin` and
`deluge-v1_3_0-rudement+2026_08_08-a99b641b.bin`.

Bypass behaves. The LFO stays locked to the timeline across a toggle — the chop resumes where
it would have been rather than restarting from phase zero, which was the main thing at risk in
the free-running phase design. No tick or click on toggle, so zeroing the SVF state on bypass
is doing its job.

Closing.
