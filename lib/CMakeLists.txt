cmake_policy(SET CMP0135 NEW)

include(FetchContent)

FetchContent_Declare(argon
  #GIT_REPOSITORY https://github.com/stellar-aria/argon
  #GIT_TAG caa7cd8056351d4fea353c1e572fa62e04a49971
  SOURCE_DIR /root/work/argon
)
FetchContent_MakeAvailable(argon)

