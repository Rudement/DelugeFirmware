## Sear: rename from Heat, automatic level matching (1.3)

Renames the per-voice clipper from Heat to Sear throughout, XML attributes included, and
replaces the static makeup constant with two one-pole followers tracking mean `|sample|` either
side of the curve, correcting on the ratio. This tracks a ratio, not a threshold — it is not a
compressor and must not become one.

`Sound` carries a seed of the converged state because Voices do not survive a note:
`acquireVoice()` places a new Voice per note-on, so detector state held only there would start
cold every note. Voices still keep their own live copy, since Sear's drive is a patched
per-voice param.

**Base:** `134d000f` → `feat/gristle-13` (`6c02c5d6`) → this.

### Depends on

- **`feat/gristle-13`** — this commit modifies `gristle.hpp`, which does not exist at the bare
  1.3 base.

### Not yet done

- [ ] **Untested on hardware.** See linked issue.
- [ ] No read-side XML alias for `heat` → `sear`.
