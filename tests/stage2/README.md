# Stage 2 chain semantics

Standalone, same reasoning as `tests/stage1` — the `tests/` project needs `-m32` and CppUTest,
this needs neither.

`MidiFxChain` is the only thing in Stage 2 with behaviour. Everything else is plumbing whose
correctness only shows up on hardware. So this pins the four things the chain decides:

- a note nobody swallows reaches every effect
- a swallowed note stops the chain dead, and later effects never see it
- `tick()` returns the **soonest** event across the chain, not the last — getting this wrong
  makes an effect fire late and is nearly invisible in testing
- overflow past `kMaxEffects` drops silently rather than faulting in the audio path

Unlike `tests/stage1`, this one includes the real header rather than copying it, so it cannot
go stale.

**As of stage 6 this no longer builds on a host g++ older than 12.** `MidiFxChain` now holds a
`RetrigFx` by value, and `retrig_fx.h` includes `arpeggiator.h` for the complete `ArpNote` type,
which reaches `<expected>`. The chain logic it checks has not changed since it last passed; it
needs a newer host compiler to run again, not a rewrite.

`VelocityFx::onNoteEvent` is stubbed in the test file. `MidiFxChain` owns a `VelocityFx` by
value so linking needs the symbol, but `velocity_fx.cpp` includes `arpeggiator.h`, which reaches
`<expected>` and needs g++ 12 or newer. The stub keeps this test about the chain, which is all
it claims to test; `tests/stage3` covers the effect itself and stubs in the other direction.

```
g++ -std=c++20 -Wall -Wextra -I ../../src -I ../../src/deluge -o /tmp/t \
    midi_fx_chain_test.cpp ../../src/deluge/modulation/midi_fx/midi_fx_chain.cpp && /tmp/t
```
