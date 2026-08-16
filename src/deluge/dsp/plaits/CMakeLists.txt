# Mutable Instruments Plaits DSP, vendored.
#
# Built as its own static library rather than folded into deluge_dsp for three
# reasons, all of which matter if this ever needs re-vendoring from upstream:
#
#  1. deluge_dsp is a UNITY_BUILD. Concatenating 40 upstream translation units
#     that each do `using namespace std;` into one TU is asking for collisions
#     we would then have to fix in vendored files.
#  2. Plaits needs GNU extensions on (M_PI is hidden under strict ANSI) and
#     upstream's own -std, which is not the Deluge's C++23.
#  3. Keeping the include root at this directory means every upstream
#     `#include "plaits/..."` and `#include "stmlib/..."` resolves unchanged,
#     so the vendored tree stays byte-identical to upstream and re-vendoring is
#     a copy rather than a merge.
#
# Upstream: https://github.com/pichenettes/eurorack (plaits, firmware 1.2)
# Licence: MIT, see LICENSE-MIT and PROVENANCE.md in this directory.

file(GLOB_RECURSE plaits_SOURCES
  CONFIGURE_DEPENDS
  ${CMAKE_CURRENT_LIST_DIR}/plaits/*.cc
  ${CMAKE_CURRENT_LIST_DIR}/stmlib/*.cc
)

add_library(plaits_dsp STATIC ${plaits_SOURCES})

# Include root is this directory, so "plaits/dsp/voice.h" and "stmlib/stmlib.h"
# resolve exactly as they do upstream.
target_include_directories(plaits_dsp PUBLIC
  ${CMAKE_CURRENT_LIST_DIR}
)

set_target_properties(plaits_dsp
    PROPERTIES
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED ON
        # ON, not OFF: strict ANSI hides M_PI, which stmlib/dsp/filter.h and
        # plaits/dsp/oscillator/sine_oscillator.h both use.
        CXX_EXTENSIONS ON
)

target_compile_options(plaits_dsp PRIVATE
    ${DELUGE_COMMON_COMPILE_OPTIONS}
    # Same rationale as deluge_dsp: unity/LTO interactions with arm/thumb.
    "-fno-lto"
    "-finline-functions"
    "-marm"
    # Upstream is warning-noisy by our standards and we do not want to edit it.
    "-w"
)
