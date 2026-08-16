
file (GLOB_RECURSE deluge_dsp_SOURCES
  CONFIGURE_DEPENDS
  ${CMAKE_CURRENT_LIST_DIR}/*.c
  ${CMAKE_CURRENT_LIST_DIR}/*.cpp
  ${CMAKE_CURRENT_LIST_DIR}/*.s
)

add_library(deluge_dsp STATIC ${deluge_dsp_SOURCES})

target_include_directories(deluge_dsp PRIVATE
  ${CMAKE_CURRENT_LIST_DIR}/..
  ${SHARED_INCLUDE}
)

target_include_directories(deluge_dsp PUBLIC
  ${CMAKE_CURRENT_BINARY_DIR}
)

set_target_properties(deluge_dsp
    PROPERTIES
        C_STANDARD ${DELUGE_C_STANDARD}
        C_STANDARD_REQUIRED ON
        CXX_STANDARD ${DELUGE_CXX_STANDARD}
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS ON
        UNITY_BUILD true
)

target_link_libraries(deluge_dsp PRIVATE
    NE10
    eyalroz_printf
    argon
)

target_compile_definitions(deluge_dsp PUBLIC
        $<$<CONFIG:DEBUG,RELWITHDEBINFO>:IN_HARDWARE_DEBUG=1>
        $<$<CONFIG:DEBUG,RELWITHDEBINFO>:ENABLE_TEXT_OUTPUT=1>
)

target_compile_options(deluge_dsp PRIVATE
    ${DELUGE_COMMON_COMPILE_OPTIONS}
    # use a unity build instead (combine all cpp to one before compiling), which has similar gains without the arm/thumb issues
    "-fno-lto"
    # we're not using lto so go ahead and inline
    "-finline-functions"
    # Build dsp code with arm mode
    "-marm"
)

# stmlib (vendored, MIT). Shared support code for both Mutable ports below --
# see stmlib/CMakeLists.txt for why it is one target, not two.
add_subdirectory(stmlib)

# Mutable Instruments Plaits (vendored, MIT). Separate target -- see
# plaits/CMakeLists.txt for why it is not folded into this unity build.
add_subdirectory(plaits)
target_link_libraries(deluge_dsp PUBLIC plaits_dsp)

# Mutable Instruments Clouds / Parasites (vendored, MIT). Separate target --
# see clouds/CMakeLists.txt for why it is not folded into this unity build.
# clouds_adapter.cpp, compiled as part of deluge_dsp itself, is what actually
# calls into clouds_dsp.
add_subdirectory(clouds)
target_link_libraries(deluge_dsp PUBLIC clouds_dsp)
