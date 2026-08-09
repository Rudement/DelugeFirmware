## Label 1.3 builds as 1.3.0-rudement

Mirrors `568127fd` on chopin.

`PROJECT_VERSION` stays 1.3.0 deliberately — it feeds `BUILD_VERSION_STRING_SHORT`, which
`storage_manager.cpp` writes into every saved song as `firmwareVersion`. Changing it would make
songs saved here claim a version no official firmware ever reported. `DISPLAY_VERSION` affects
only the build filename and the on-device version readout, so songs stay byte-identical to stock.

**Base:** `134d000f`. No prerequisite branch.

**Do not send this upstream** — it is a fork identity label.
