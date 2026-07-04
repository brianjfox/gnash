# Copyright (c) 2026 Brian J. Fox
# Licensed under GPLv2 with the GPLv2-AI Exception.

# Common warning configuration, carried as an INTERFACE target so individual
# libraries opt in with `target_link_libraries(<tgt> PRIVATE gnash_warnings)`.
add_library(gnash_warnings INTERFACE)

target_compile_options(gnash_warnings INTERFACE
  -Wall
  -Wextra
  -Wshadow
  -Wnon-virtual-dtor)

if(GNASH_WERROR)
  target_compile_options(gnash_warnings INTERFACE -Werror)
endif()
