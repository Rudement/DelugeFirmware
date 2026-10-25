# USB Audio Feature Guide

## Controls

*Sending a track out* — open a synth, audio or kit clip, press SELECT, and choose `USB`. Each item is an on/off switch, and any number can be on at once.

| Item | What it does |
|---|---|
| `1` – `8` | Sends this track to that USB channel, mixed down to mono |
| `1-2`, `3-4`, `5-6`, `7-8` | Sends this track to that pair in stereo: left on the first channel, right on the second |
| `MAIN` | On by default. Off removes the track from the Deluge's own outputs, so it is heard only through USB |

Routing belongs to the track, so launching another clip on the same track keeps it. Several tracks sent to the same channel are mixed together there. A kit is routed as a whole. The routing is saved with the song.

*Taking audio in on a track* — on an audio track, open the input selector, choose `USB`, then press SELECT to pick:

| Item | What it does |
|---|---|
| `1` – `4` | That returning channel as the track's input, in mono on both sides |
| `1-2`, `3-4` | That returning pair as the track's input, in stereo |

The track then plays and records the returning audio like a line input, through its own filters, effects, volume and pan. A pair used by a track is not also added to the song mix by `RTN`.

*Settings* — `SETTINGS > USB`. These are saved on the Deluge, not in the song.

| Item | What it does |
|---|---|
| `RTN` | Which returning pair is mixed into the song: off, `1-2` or `3-4`. It joins before the song's effects, master filter, volume and compressor, so it is treated like the Deluge's own tracks |
| `RLVL` | Level of the `RTN` pair, 0–50 in 1.2 dB steps. 0 is silent, 40 is unchanged, 50 is +12 dB |
| `CMP` | Delay compensation, 0–100 ms. Holds the Deluge's own tracks back by this much so a track sent out to an external effect and returned lands in time with the rest. Set it to the round-trip delay of your external setup. 0 is off |

## How it works

- A track's USB copy is the difference its render makes to the mix, so nothing is rendered twice. Each routed destination costs about 0.2% of the processor.
- Returning audio passes through a 512-frame (~11.6 ms) buffer. Round trip on the Deluge's side is about 20 ms; the computer adds its own buffering on top.
- Delay compensation delays the Deluge's summed tracks and their reverb send. Audio arriving over USB is not delayed.
- 8 out and 4 in is the most a Full Speed USB connection carries at once at 16-bit, 44.1 kHz.

## Known limitations
- While connected as a USB audio device, the Deluge cannot host a USB MIDI controller. MIDI to and from the computer still works.
- Tracks are sent dry and before the master effects and volume.
- A track taken out of the main mix still sends its reverb tail to the main outputs.
- Kits route as a whole, not per drum. MIDI and CV tracks have no USB routing.
- On a dense song, routing costs about 1.5 voices.
- The return drops about one sample a second to stay in step with the computer's clock. This is inaudible.
- With CMP on, everything you play on the Deluge is heard that much later. Line inputs are delayed too.
