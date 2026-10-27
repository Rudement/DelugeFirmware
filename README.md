# Rudement Firmware for the Synthstrom Deluge

Rudement is a fork of the [Deluge Community Firmware](https://github.com/SynthstromAudible/DelugeFirmware),
built on community release **1.3 RC2**. It adds new instruments, effects, MIDI tools and live-performance
features. Almost every addition has its own On/Off switch under
`SETTINGS > COMMUNITY FEATURES > Rudement upgrades`, so you can turn on only what you want.

## What's in it

- **Sound:** Bouls (Mutable Instruments Plaits oscillator), Wisps (Mutable Instruments Clouds texture effect),
  Four-Band EQ, Delay & Reverb Filters, Sear, Gristleator
- **Song and grid:** Grid Column Reorder, Track Edit, Altered-Note Shading
- **Instruments and kits:** Kit Split, Chroma
- **MIDI and sequencing:** MIDI FX (MIDI Delay, Strum, Spread, Spice & Dice, Stochastic, Harmonize, Generate), Groove
- **Live performance:** Temp Hold, Morph, Loop Remix, Scene Capture, MIDI Follow Grid Hold-to-Lock
- **System:** OLED Visualizer, USB Audio (8 out / 4 in), SETTINGS > CPU load page

The full operating manual is in [`manuals/`](manuals/) and attached to each release.

## Installing

1. **Back up your whole SD card** to your computer first. Songs saved with newer firmware may not open on older firmware.
2. Download the `.bin` file from [Releases](../../releases).
3. Copy it to the top level of the SD card (not in a folder). Make sure it's the only `.bin` there.
4. Put the card back in the Deluge, **hold SHIFT and switch it on**, and keep holding until it has started up.
5. Check `SETTINGS > FIRMWARE VERSION`. It should read `1.3.0-rudement`.

To go back, put your previous `.bin` on the card instead and repeat step 4. Chapter 00 of the manual has the details.

## Building from source

This repository builds the same way as the upstream firmware. See
[Getting Started with Deluge Development](https://delugecommunity.com/development/deluge/getting_started/),
then run `./dbt build release` (or `dbt build release` on Windows). The `.bin` lands in `build/Release/`.

## Support

If you enjoy this firmware, you can support its development on [Patreon](https://www.patreon.com/Rudement).

## Credits

- **Rudement**: created and maintains this firmware, its features and the manual
- **Synthstrom Audible** and the Deluge community developers, for the open-source firmware this is built on
- **Émilie Gillet / Mutable Instruments**: Plaits and Clouds DSP, MIT licence (see `src/deluge/dsp/plaits/` and `src/deluge/dsp/clouds/`)
- **sticknobills**: USB Audio, adapted for this firmware ([original](https://github.com/sticknobills/DelugeFirmware))
- **Bruce Zawadzki (Tone Coder / bzawad)**: OLED Visualizer, adapted for this firmware
- **Commander Cody**: Chroma, adapted from his unofficial Chroma firmware

## Licence

GPL-3.0, the same as the Deluge firmware. See [LICENSE](LICENSE). Third-party code keeps its own licence notices.
