# Mutable Instruments Clouds (Parasites) DSP, vendored.
#
# Same three reasons as plaits/CMakeLists.txt for being a separate static
# library rather than folded into the deluge_dsp unity build: collision risk
# from concatenating upstream translation units, upstream's own -std/GNU
# extensions requirement, and keeping the include root here so
# `#include "clouds/dsp/..."` resolves unchanged and re-vendoring stays a
# copy rather than a merge.
#
# Upstream: https://github.com/mqtthiqs/parasites (clouds/)
# Licence: MIT, see LICENSE-MIT and PROVENANCE.md in this directory.

file(GLOB_RECURSE clouds_SOURCES
  CONFIGURE_DEPENDS
  ${CMAKE_CURRENT_LIST_DIR}/clouds/*.cc
)

add_library(clouds_dsp STATIC ${clouds_SOURCES})

# Include root is this directory, so "clouds/dsp/granular_processor.h"
# resolves exactly as it does upstream. "stmlib/..." resolves via
# stmlib_dsp's own public include directory, picked up through the PUBLIC
# link below.
target_include_directories(clouds_dsp PUBLIC
  ${CMAKE_CURRENT_LIST_DIR}
)

target_link_libraries(clouds_dsp PUBLIC stmlib_dsp)

set_target_properties(clouds_dsp
    PROPERTIES
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED ON
        # ON, not OFF: strict ANSI hides M_PI, which several Clouds headers
        # and stmlib/dsp/filter.h use.
        CXX_EXTENSIONS ON
)

target_compile_options(clouds_dsp PRIVATE
    ${DELUGE_COMMON_COMPILE_OPTIONS}
    "-fno-lto"
    "-finline-functions"
    "-marm"
    # Upstream is warning-noisy by our standards and we do not want to edit it.
    "-w"
    # granular_processor.cc initialises Parameters' `freeze` and `gate` bools
    # from 0.0f in a braced-init-list. That was fine in the C++98/03 dialect
    # Mutable wrote against; C++11 onwards makes narrowing in a braced init
    # ill-formed, and gcc reports it as an ERROR, which plain "-w" does not
    # suppress. The alternative -- editing two literals to `false` -- would
    # break the byte-for-byte identity with upstream that PROVENANCE.md
    # promises and turn every future re-vendor into a merge, so it is a
    # compiler flag instead. Scoped to this target; nothing of ours is built
    # with it.
    "-Wno-narrowing"
)

target_compile_definitions(clouds_dsp PRIVATE
    # Upstream's own escape hatch: with TEST defined,
    # clouds/drivers/debug_pin.h compiles to empty inline stubs instead of
    # touching stm32f4xx_conf.h GPIO registers. See PROVENANCE.md -- nothing
    # else taken from upstream is gated on TEST.
    TEST
)
