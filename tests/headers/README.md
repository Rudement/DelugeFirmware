# Header parse check

The `modulation/midi_fx/` headers are written to need nothing from the firmware but forward
declarations. That is worth keeping true, and worth checking, because it is the one part of this
feature that a host compiler can verify completely — everything else reaches `arpeggiator.h`,
which reaches `<expected>` and needs g++ 12 or newer.

Parses the standalone ones under `-Wall -Wextra -Wpedantic` and asserts the sentinel matches the
one the arp and the schedulers use, and that the settings object stays small — it is now stored
per clip *and* per drum.

**Two headers are deliberately excluded, and that exclusion is the point.** `retrig_fx.h` owns
`ArpNote` records by value, so it needs the complete type and includes `arpeggiator.h`;
`midi_fx_chain.h` holds a `RetrigFx` and inherits it. That happened when stage 6 became the first
effect to emit note-*ons* — a note-off needs only a code and a channel, a note-on needs a real
`ArpNote`.

Nothing new is dragged anywhere, since every file including those two already includes
`arpeggiator.h`. But the property is genuinely gone for them, and deleting this test rather than
narrowing it would have thrown away the signal for the headers where it still holds.

Parse-only, deliberately. Instantiating an effect needs its vtable, which lives in a `.cpp` that
includes `arpeggiator.h` and so will not build on a host compiler older than g++ 12. The
static_asserts also pin the three defaults that make an old song load inert.

```
g++ -std=c++20 -Wall -Wextra -Wpedantic -I ../../src -I ../../src/deluge \
    -fsyntax-only midi_fx_headers_test.cpp
```
