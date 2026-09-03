# AUX Sends, back on the 1.3 line — 2026-09-02

Branch: **`feat/aux-sends-13-redux`**, eleven commits on top of `feat/chroma-on-13`
(`6cc08f26`).

**Built 2026-09-01 22:00 (local).**
`AUX13-deluge-v1_3_0-rudement+2026_09_01-aa627731.bin` — 2,061,572 bytes, in the repo root.
`BUILD_EXIT=0`, zero errors, three warnings and none of them in any file this work touches.
560 objects from a wiped build directory on toolchain v22 (GCC 14.2.1), 996 s including a
200 s LTO link. 30,068 bytes larger than `CHROMA-13-FIX6` at the same base, which is the
sends and their menus. The repo is left checked out on this branch.

**Base checked 2026-09-02 04:2x.** `6cc08f26` is still the newest point on the 1.3 line —
nothing in the repo is ahead of it except this branch. The 1.2.1 work of the same evening
(`ed8af794`, MIDI Follow CCs and the LEARN-gated Harmonic Brush) is 1.2.1 *catching up to*
1.3: the Gristleator on 114–119 + 92/94/95, the EQ mids on 33/34/35 and both Harmonic Brush
LEARN commits are all already in `6cc08f26`, verified in `midi_follow.cpp` rather than taken
from the commit message. Nothing to fold in.

**The build directory has to be wiped.** It was last configured for the 1.2.1 line (toolchain
v16); 1.3 wants v22 and they share one `build\`. `run-build-aux-13.cmd` does the wipe, so
expect a full rebuild and a dependency re-fetch. Going back to a 1.2.1 branch afterwards needs
the same wipe again.

**Instrumented build, 2026-09-01 23:5x — `feat/aux-sends-13-diag` (`0a12cbb7`).**
`AUXDIAG-deluge-v1_3_0-rudement+2026_09_01-0a12cbb7.bin`, 2,063,556 bytes. `BUILD_EXIT=0`.
Two hardware sessions failed in ways reading the code did not predict, so this build measures
instead of guessing. It changes nothing about how the handover works.

*What went wrong on hardware, from the first build:* the Deluge froze on roughly half of
boots — screen frozen on the last frame, no error code, nothing responding, which is the
shared SPI bus jamming rather than a fault the firmware caught. And the socket produced only
pops that spiked and decayed, correlated with turning a knob. Pops are DC steps at the
converter, not mangled audio: the ring is not being played, rather than being played wrong.
A knob turn redraws the screen, which forces a bus handover — so what is audible is very
likely the handover itself. **So the first fix was not the whole fault.** The race it closed
is real, but something else is also wrong.

Ruled out by reading, so nobody rediscovers them: the render window is 128 samples against a
256 limit, so capture is never skipped for size; and the per-drum send is inert by design, so
a send set on a drum row was never going to make a sound (kit-global is the one that works).

*Reading the counters,* at SETTINGS → AUX → STATS, after playing a synth track with a send up:

| what you see | what it means |
|---|---|
| **DMA stalls** large | the transfer engine is not consuming the ring — the converter is being fed nothing, and everything else is beside the point |
| **Windows dropped** ≈ or > **Windows written** | the audio is discarded before delivery; the fault is the pump dropping a window whenever the display holds the bus, not the arbitration |
| **Bus yields** running away from **take backs** | the bus is handed over and not handed back — the deadlock, and the thing to check after a freeze |
| **Stream starts/stops** both large | the render path is changing its mind every window, a fault in its own right |

STATS stays visible with the feature switched off, so turning AUX Sends off does not destroy
the evidence. Known cosmetic issue: eight `-Wvolatile` deprecation warnings from `++` on the
counters. Harmless, worth tidying on the next pass.

**The Community Features toggle is now a real off switch.** It gated the menus alone before,
so switching AUX Sends off hid the controls while the capture and stream carried on — and
removed the menu you would have used to turn the sends down. `cvOutputsAvailable()` now reads
it, so with the feature off nothing is routed, the stream stops on the next window, and the
sockets go back to note voltages.

**First readings off the instrumented build — and they exonerate the arbitration.**

| counter | value | what it says |
|---|---|---|
| Stream starts / stops | 1 / 0 | started once, never stopped. The send reaches the render path and stays seen. The param plumbing is correct. |
| Bus yields / take backs | 253 / 256 | balanced, and *tiny* — a few hundred handovers across the whole session. No deadlock, no thrash. The display is barely touching the bus. |
| Windows written / dropped | 821,574 / 26,079 | 3.2% dropped. The pump is writing audio into the ring essentially all the time. Not starvation. |
| DMA stalls | 451,218 of 821,574 | **54.9%.** The read pointer had not moved since the previous window, on more than half of them. |

At 128 samples a window the pump runs every ~2.9 ms, in which the transfer engine should consume
about 136 frames. The expected stall count is zero. Fifty-five percent is the anomaly, and it is
the only one.

So: the audio is being written into the ring, the bus is not being fought over, and the converter
is not consuming what is there. **Every arbitration theory — the original commit's, and the
one-step fix on top of it — was aimed at the wrong layer.** The fault is in the transport: either
the SPI is not clocking the words out at anything like the rate it should, or the converter is not
latching them, in which case the DC steps heard as pops are all that path can ever produce.

Two readings of the 55%, and they need different fixes:

- **The transfer really is running far below rate.** Then the writer laps the reader constantly,
  the lead measurement is out of range every window, and Resyncs should be enormous — far beyond
  the ~253 the handovers can account for.
- **CRSA_n is simply not updated continuously by the hardware**, and the boolean stall count is a
  coarse instrument measuring nothing. Then Resyncs stays near the handover count.

**Resyncs came back 48,161.** That is 190x what 253 handovers can account for, so the lead is
leaving its 640-frame window on its own about every seventeenth window. Writer and reader
disagree about rate; the arithmetic on those two numbers puts the converter somewhere near 70%
of what the pump assumes. Near, though — and a rate error alone would give pitched-down audio,
not silence, so this is measured rather than inferred in the next build.

**Second instrument — `feat/aux-sends-13-diag2` (`44ba5af3`).** Four more numbers under the
same menu:

| counter | correct value | what a wrong one means |
|---|---|---|
| **Advance x10** | 1360 | frames the transfer engine consumed per window, x10. A correct 47 kHz stream against a 128-sample window is 136.0 frames. |
| **Emit x10** | ~1360 | frames the pump emitted over the same windows. Advance ÷ Emit *is* the rate error. |
| **SPI status bits** | 96–224 | sticky OR of the RSPI status register. **Bit 2 (add 4) is MODF, a mode fault** — something else drove the chip-select while the block was master, and the block stops transmitting. Bit 0 (add 1) is OVRF, overrun. Bits 5/6/7 (32/64/128) are the ordinary idle flags. |
| **Lead now** | near 768 | parked at an extreme means the loop never converges. |

Three outcomes, three different fixes: MODF set is the contested SSL pin and the answer;
Advance well below Emit is a confirmed rate deficit; Advance ≈ Emit ≈ 1360 means the transport
is fine and the converter is not latching, which is word framing and somewhere else entirely.

DmaStalls is now derived from the same distance measurement. The eight `-Wvolatile` warnings
are gone — the counters increment by assignment.

Next instrument, if Resyncs does not settle it: record the *distance* CRSA moves per window rather
than whether it moved, and latch the RSPI status register's error flags. A mode fault there would
be decisive — that is what a contested SSL pin looks like, and it would stop transmission and
stall the DMA exactly like this.

## ROOT CAUSE FOUND — two DMA channels claiming one transmit request

`feat/aux-sends-13-fix2` (`0d346c94`). The counters found it; no further reading of the code
would have.

Measured with a send up on a synth track: **Advance x10 = 0** — frames the transfer engine
consumed per window, zero, not slow. **Emit x10 = 117**, which is correct (11 samples a window
times the 47/44.1 ratio, so the pump is doing its job). **Lead = 1278** against a target of 768,
which is what a writer feeding a reader that never moves looks like. **SPI status = 128**: SPRF
only, SPTEF never once set, so the block was never transmitting.

`oledDMAInit()` assigns `DMARS_FOR_RSPI_TX` to channel 4 at boot and leaves it there.
`initDMAWithLinkDescriptor()` assigns the same MID/RID to channel 5 on every engage. `setDMARS`
writes only its own half of the DMARS2 register the two share, so **both assignments survive** —
and the RZ/A1 manual forbids it: a request resource belongs to one channel. With two claiming
it the request reaches one of them, and the measurement says it is not ours.

**This is the whole 7SEG-works / OLED-does-not split.** On a 7-segment machine `oledDMAInit()`
never runs, channel 4 never claims the resource, channel 5 has it uncontested. Which also means
the original commit's "measured flat to ~4.7 kHz" was true and useless here — measured on the
one model where the collision cannot happen.

The handover arbitrates the SPI block, the chip-select pin and the transfer engine. It never
arbitrated the request assignment, which is the one thing deciding whether the transfer engine
is asked to move at all. Both earlier fixes — the original design's, and the interrupts-off one
on top of it — were correct about ordering and beside the point.

The fix puts the assignment into the handover: engage releases the display's claim before taking
ours, release gives it back after dropping ours, never both at once. The return leg matters as
much as the claim; without it the display's own frames would stop the moment the stream first ran.

**What success looks like, in the same counters:** Advance x10 comes up to meet Emit x10, DMA
stalls fall away, Lead sits near 768 instead of drifting to the threshold. Not tested on hardware.

---

---

## 1. The finding that matters

The revert (`c384eaa3`) said the freeze was "root-caused to the SPI-bus-sharing
yield/resume handshake". That was a **bisection result, not a reading** — and the
bisection could not have been more specific than it was, because on an OLED machine the
bus-sharing commit is also the commit that first lets the stream run at all. Before it,
`cvOutputsAvailable()` returned `!have_oled_screen`: every line of AUX Sends was inert on
your Deluge. So "the handshake commit" and "all of AUX Sends, first time it ever
executed here" were the same bisect step.

Reading it, the defect *is* in the handshake, and it is specific.

**The bus has two owners and two flags, and only the flags were guarded.** The display's
claim is `spiBusCurrentlySending`, which `enqueueOLEDFrame()` and `enqueueCVMessage()`
test with interrupts off before touching the SPI block. The stream's is
`cvStreamYielded`. But the register and pin work each flag stands for ran *outside* the
guard.

So the two sequences interleave. `oledDeselectionComplete()` clears
`spiBusCurrentlySending` and only *then* calls `cvStreamYieldEnd()` — so for the whole
length of `cvStreamEngageBus()` the bus reads as free while it is being set up. Anything
arriving in that window claims it and runs `cvStreamReleaseBus()` through the same
registers and the same chip-select pin, halfway through the engage. What is left is a
self-linking DMA feeding a block the display believes it owns, with the stream marked
yielded so nothing ever takes it back down. The display's transfers stop completing.

**Why it presented as a file-load bug.** `FileReader::readDone()` — storage_manager.cpp,
the song-load read loop — calls `AudioEngine::routineWithClusterLoading()`,
`uiTimerManager.routine()` and `oledRoutine()` back to back every 64 reads. For the whole
length of a load, the render path's stream starts and stops, the display's frames and the
CV note words run into each other continuously. The window is microseconds wide; that
loop is the one place that hits it thousands of times a second. Nothing about file *parsing*
was ever wrong.

## 2. The fix

One commit, one file: the `CriticalSectionGuard` now covers the hardware as well as the
flag, in all four places that move the bus — `cvStreamYieldBusToDisplay()`,
`cvStreamTakeBusBack()`, `cvStreamStart()`, `cvStreamStop()`. With interrupts off no
display path can run, so the claim cannot change underneath a handover and no competing
register write is possible. The ordering of flag against hardware then stops mattering,
which is why **nothing in `oled_low_level.c` changes**.

Cost: the two waits in `cvStreamReleaseBus()`, one 32-bit word each at the streaming
rate — about 20 µs, against an audio window of a few milliseconds. `enqueueCVMessage()`
has always called the release from inside its own critical section, so one of the four
paths has been paying this all along.

`kCvBusWaitSpins` drops 20000 → 2000. It was sized when the only risk was hanging an
interrupt handler; it now sets the ceiling on how long interrupts stay off.

## 3. What else is on the branch

The nine original commits, cherry-picked clean except two:

| | resolved how |
|---|---|
| `Community Features: a toggle for AUX Sends` | additive against the five toggles added since (Keyboard Note Preview, Scene Capture, Grid Column Reorder, Chord Brush, Retrospective Capture). `g_english.cpp` / `g_seven_segment.cpp` **regenerated** from the JSONs, not hand-edited — they came back one line different from the tracked files and identical otherwise, so nothing had drifted. |
| `separate menu visibility from socket capability` | the toggle commit had already introduced `auxMenusVisible()`; it now reads `cvSendMenusVisible() && toggle` rather than `cvOutputsAvailable() && toggle`. Code identical to the branch version; comments taken from it verbatim. |

Checked, not assumed:

- **Automation array sizes.** 100 non-global / 57 global, counted against the actual
  entries rather than the comment arithmetic. A short initialiser list is legal C++ and
  pads with phantom params, so this is the check that matters, not the compile.
- **Shortcut grids.** No duplicate param in any of the four tables; the four CV params
  hold one pad each.
- **Clouds param IDs unchanged.** The CV masters land *after* the Clouds block, so
  nothing shifts under existing automation. The CV *sends* sit in the shared block and
  push the arp params up by two — songs save by name, so that is cosmetic.
- Outside `routing.h` and `cv_audio_stream.cpp`, the tree is byte-identical to
  `feat/aux-sends-oled-kitsplit-13`. `param.h`, `param.cpp`, `clip.cpp`,
  `oled_low_level.c` and the rest are the versions that branch already carried.

The Community Features toggle defaults **on**, as it did on the branch.

## 4. Test plan — load a song first

The build has not been compiled, let alone run. Test in this order, because the first
step is the one that failed before:

1. **Flash, then load a song. Any song, with no sends set.** This is the old failure with
   the stream never running — it should be indistinguishable from the current build.
2. **Set CV1 Send up on one track, save, load it back.** This is the actual test: the
   stream is running while the song file is read. If it freezes here, the fix is wrong
   and section 6 is where to look next.
3. **Load repeatedly while a send is up** — a big song, several times. The window is a
   race; one clean load is not proof.
4. Only then the feature itself: audio at the socket, MAIN off moving a track off the
   mains, a second track summing into the same socket, `SETTINGS → AUX` levels, Stereo
   Split collapsing two sends to one.
5. **Menu-dive during a take.** A tick per redraw is expected and documented; a gap that
   grows, or silence that never comes back, is not.

## 5. Way back

Unchanged. `_gigsafe\KNOWN-GOOD-currently-on-deluge-BISECT7-auxbare-06a0d8e9.bin`, or the
current `CHROMA-13-FIX6-…-6cc08f26.bin` for the 1.3 line with everything except this.

In the firmware itself: **SETTINGS → COMMUNITY FEATURES → AUX Sends → off** takes the
menus away without reflashing. With every send at zero the stream never starts, so a boot
into a song that does not use the sockets is inert either way.

## 6. If it still freezes

The fix is deliberately one small commit so this stays answerable. Next suspects, in
order:

- `cvStreamPump()` reads `cvStreamYielded` and the DMA read pointer unguarded. A stale
  read there costs one window's lead estimate, which `cvStreamResyncPending` covers — but
  it is the remaining unsynchronised reader.
- The lost-DMA-interrupt watchdog in `oledRoutine()` re-drives `oledTransferComplete()`
  after 50 ms. It was written for issue #3670 and has never run against a CV stream on
  the bus.
- `cvStreamStart()` fills and cache-invalidates the whole 16 KB ring. During a load
  `cvStreamRenderComplete()` can start and stop the stream on alternating windows; that
  is not a hang, but it is a lot of work in the wrong place.

## 7. Follow-up, not done

Manual chapter 08 still opens with "Aux Sends lives on the `feat/aux-sends-*` branches.
It is not on the Clouds / Kit Split line, where it was reverted…". Once this is confirmed
on hardware, that paragraph and figure 1.1 need rewriting. The tracked manual is only on
the 1.2.1 line.


---

# 2026-09-03 — forwarded onto the 1.3.0 beta, and an intermittent hard fault

Branch is now **`feat/aux-sends-13-gated`** at `cc32102b`: the nineteen AUX commits with
`integration/beta-13-2026-09-01` merged under them. Thirty-seven commits came in, no conflicts,
because the beta touched neither `song.cpp`, `audio_engine.cpp` nor the sample types and the
AUX work lives in those and in its own `cv_audio_stream` files.

Build: `AUXGATED-deluge-v1_3_0-rudement+2026_09_02-cc32102b.bin`, 2,117,488 bytes, `BUILD_EXIT=0`,
560 objects, toolchain v22.

## The fault

Two hard CPU aborts across the evening, both read off the pad grid (see the decoding table in
`docs/DEVELOPMENT-NOTES.md`), both with the display still showing the auto-loaded song, so both
after load and once audio was rendering:

| build | SYS LR | function |
|---|---|---|
| `cc32102b` | `0x20141842` | `Song::renderAudio [.constprop.0] +1174` |
| `cf8f54f2` | `0x20139050` | unknown — that build's symbols had been wiped |

`renderAudio +1174` is inside the send capture. That is the whole reason this is filed here.

## It is a race, and the merge is not the cause

Both builds then booted **5/5** with the same song loading, having each faulted once earlier.
Roughly one bad boot in six, no change in between, so "did it boot" cannot measure it and no
single boot proves anything. Two consequences worth not relearning:

- **The beta merge is exonerated.** `cf8f54f2` faults too. So are the new optimiser flags
  `dc0c7491` brought in (`-ffast-math`, `-fsplit-loops`, `-funswitch-loops`) — the branch
  `test/aux-13-noflags` was built to test them and is now moot. Delete it or leave it; the
  commit message says why it exists.
- **`song.cpp` and `cv_audio_stream.cpp` are byte-identical across the merge.** Verified by
  hash, not by reading the diff. Whatever this is, it predates the beta.

## Candidates checked and eliminated by reading

Do not spend another evening on these:

- **Scratch buffer overrun.** `fitsScratch` guards `numMono <= 512` against
  `cvCaptureScratch[kCvMaxWindow * 2]` with `kCvMaxWindow = 256`, and the beta left both that
  and `SSI_TX_BUFFER_NUM_SAMPLES = 128` alone.
- **Re-entrant render clobbering the shared scratch.** `AudioEngine::audioRoutineLocked`
  already prevents a nested `routine()`, so the single global scratch cannot be entered twice.
- **A dangling `Clip` through `lastSendGain`.** `cvStreamCapture` reads and writes that array
  within the call and does not retain the pointer.
- **Indexing a sound-sized param set with a global-only param.** `UNPATCHED_CV1_SEND` and
  `UNPATCHED_CV2_SEND` sit below `UNPATCHED_NUM_SHARED`, so they exist in both layouts. Only
  `UNPATCHED_CV1_MASTER` is global-only, and it is read from the Song, which is a
  GlobalEffectable.

So the capture block does not obviously contain the bug. It needs instrumenting, not reading —
which is what `feat/aux-sends-13-diag` was for, and the same approach applies here.

## The fault display gives one pointer, and that is a handler limitation

Both faults showed a single blue group and nothing else. That is not luck: the abort vector
runs in abort mode, where `SP` is banked, so `handle_cpu_fault` receives `SP_abt` rather than
the program stack. `isStackPointer()` then rejects it and the stack scan finds no return
addresses at all. Expect exactly one pointer — the SYS-mode LR — from any hard fault, and do
not read anything into the absence of the rest. The `@TODO` above
`fault_handler_print_freeze_pointers` is about this.

## Keep the symbols

`build\` gets wiped between lines, and the build scripts copy out only the `.bin`. That is how
`cf8f54f2`'s symbol table was lost, and why one of the two faults above cannot be named. There
is now a `symbols/` folder in the repo root with the `.nmdump` for `cc32102b` and `f6d0faa0`.
**Copy the `.nmdump` out with every build worth flashing** — 800 KB, and without it a fault
display is just a number.
