# Pipeline test

Throwaway commit, only purpose is to prove the chain works end to end before any
real code goes in:

    edit here  ->  push to Rudement/DelugeFirmware  ->  Actions "Build"  ->  .bin

If the Build workflow produces a `dev_Release_build_bundle` artifact containing a
`.bin` of a sane size (a couple of MB — NOT 0 bytes, NOT >3.5 MB), the pipeline is
good and this file can be deleted.

This file is documentation only and changes nothing about the firmware, so a failed
build here means the pipeline is at fault, not the change.
