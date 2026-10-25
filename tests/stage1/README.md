# Stage 1 equivalence check

Standalone, deliberately not wired into `tests/CMakeLists.txt` — that project needs `-m32` and
CppUTest, and this needs neither. It exists to check one specific claim rather than to be part
of the suite.

The claim: MIDI FX Stage 1 moved the dispatch seam in `NonAudioInstrument::renderOutput` out
from behind `arpSettings->mode != ArpMode::OFF`, so with the arp off it now runs against an
`ArpReturnInstruction` no arpeggiator ever touched. That is only safe if such an instruction is
inert. If it isn't, the arp-off path starts emitting notes that were never played.

`ArpReturnInstruction`'s constructor and both `dispatchArpNote*` bodies are copied **verbatim**
from `modulation/arpeggiator.h` and `model/instrument/non_audio_instrument.cpp`. If either
changes upstream, this copy goes stale and the check becomes meaningless — re-copy or delete it.

```
g++ -std=c++20 -Wall -Wextra -o /tmp/t inert_instruction_test.cpp && /tmp/t
```
