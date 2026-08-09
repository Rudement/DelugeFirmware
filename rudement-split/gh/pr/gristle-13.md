## The Gristleizer: standalone 9-parameter effect on 1.3

Ports `403fa750` from the chopin (1.2.1) branch. One LFO driving an amplitude stage and a
filter stage, after Roy Gwinn's 1975 Practical Electronics design and Chris Carter's
front-panel Bias modification.

Nine shared unpatched params, processed in `ModControllableAudio::processFX` between ModFX and
EQ, so it appears on synths, kits, audio clips and song FX alike — unlike Sear, which is
per-voice and cannot leave a synth clip.

Deliberately **not** a ModFX type. See the commit message for why that was tried and abandoned.

**Base:** `134d000f` (1.3 line).

### This is a shared prerequisite

`src/deluge/dsp/gristle.hpp` does not exist at the 1.3 base — it is created here. Both
`feat/sear-13` and `feat/gristle-on-13` modify that file, so **both sit on top of this branch**.
This is a real dependency, not a naming artefact.

Merge or review this one first.
