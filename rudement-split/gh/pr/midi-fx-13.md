## Collapse the arpeggiator fan-out into single dispatch seams

Two commits, groundwork for a MIDI FX stage.

**`f067eac7` — MIDI/CV side.** Four call sites in `NonAudioInstrument` (`renderOutput`, both
halves of `sendNote`, `doTickForwardForArp`) each repeated the same three loops verbatim: glide
note-offs, plain note-offs, note-ons, every one stopping at the first `ARP_NOTE_NONE`. That is
every path by which an arpeggiator result reaches a MIDI or CV output. Factored into
`dispatchArpNoteOffs` / `dispatchArpNoteOns`, so there is a single place to stand between the
arp and the output — which is what the MIDI FX stage needs.

**`a99b641b` — Sound side.** The note-ON half already had a shared home in
`process_postarp_notes`. The note-offs did not; the same two loops were repeated verbatim in
`Sound::noteOff`, `Sound::render` and `SoundInstrument::doTickForwardForArp`. Now
`Sound::dispatchArpNoteOffs`. It returns whether anything was switched off, because two of the
three callers use that to clear `invertReversed` and the third does not.

**Base:** `134d000f`. No prerequisite branch — applies cleanly to the bare base.

### Note

Pure refactor. No behavioural change intended; both are seams for later work.

This is the most upstream-ready branch of the set — it depends on none of the Heat/Sear/EQ/
Gristleizer work and is a readability win on its own terms.
