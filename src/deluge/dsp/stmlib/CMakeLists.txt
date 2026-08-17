# stmlib, vendored. Shared support library used by both Mutable Instruments
# Plaits and Mutable Instruments Clouds (see ../plaits and ../clouds).
#
# Split out into its own static library, rather than living inside plaits/ as
# it did before Clouds arrived, so both vendored trees link the same compiled
# objects instead of each carrying a private copy. stmlib/utils/random.cc and
# stmlib/dsp/units.cc define real (non-template, non-inline) symbols; two
# independent copies linked into the same firmware would be an ODR violation
# at link time, not just wasted .text.
#
# Same build rationale as plaits/CMakeLists.txt: kept out of the deluge_dsp
# unity build, and given upstream's own -std and GNU extensions.
#
# Upstream: https://github.com/pichenettes/stmlib (Plaits' subset) plus
# stmlib/dsp/atan.{h,cc} and stmlib/fft/shy_fft.h, added for Clouds from
# https://github.com/mqtthiqs/stmlib @ 8ab2aaee77cbacb47b646d46d22ee5d358effe2d
# (both files depend only on headers already present here). See PROVENANCE.md
# in ../plaits and ../clouds for the full accounting.
# Licence: MIT.

file(GLOB_RECURSE stmlib_SOURCES
  CONFIGURE_DEPENDS
  ${CMAKE_CURRENT_LIST_DIR}/*.cc
)

add_library(stmlib_dsp STATIC ${stmlib_SOURCES})

# Include root is the dsp/ directory (one level up), so
# "stmlib/stmlib.h" etc. resolve exactly as they do upstream, for anything
# that links this library.
target_include_directories(stmlib_dsp PUBLIC
  ${CMAKE_CURRENT_LIST_DIR}/..
)

set_target_properties(stmlib_dsp
    PROPERTIES
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED ON
        # ON, not OFF: strict ANSI hides M_PI, which stmlib/dsp/filter.h uses.
        CXX_EXTENSIONS ON
)

target_compile_options(stmlib_dsp PRIVATE
    ${DELUGE_COMMON_COMPILE_OPTIONS}
    "-fno-lto"
    "-finline-functions"
    "-marm"
    # Upstream is warning-noisy by our standards and we do not want to edit it.
    "-w"
)
