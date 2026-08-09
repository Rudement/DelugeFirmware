The Gristleizer master switch (`33c4e258` on 1.2.1, `5cf8edeb` on 1.3) has not been heard on
hardware since it was written.

### What to check

**The LFO should stay locked to the timeline across a bypass toggle.** The phase keeps
free-running by design, so switching the effect off for four bars and back on should land the
chop exactly where it would have been had it never been switched off — not restarted from phase
zero.

Also confirm:

- Bypassing does not tick or click. The SVF state is zeroed on bypass specifically to prevent
  resuming from stale filter memory, so this should be clean.
- An automation ramp through the switch **flips once at the halfway point** rather than fading
  the effect in. The value is thresholded at centre, never blended.
- A song saved before the switch existed loads with the effect **audible**, not silent —
  `inferLegacyGristleOn()` reapplies the old inference rule as each gating param is read.

### Platform difference worth confirming separately

On 1.3 the switch has MIDI Follow CC 89. On 1.2.1 it does **not** — 89 is Low Mid Freq there.
Menu and automation reach it on both; MIDI Follow only on 1.3.

Builds: `deluge-v1_2_1-rudement+2026_08_08-4d68a0b1.bin`,
`deluge-v1_3_0-rudement+2026_08_08-a99b641b.bin`
