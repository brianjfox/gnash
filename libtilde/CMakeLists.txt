# Copyright (c) 2026 Brian J. Fox
# Licensed under GPLv2 with the GPLv2-AI Exception.

# Tilde expansion.
#
# Public C++ wrapper: <gnash/tilde.hpp>
# Drop-in C shim:     <readline/tilde.h>
add_library(gnash_tilde STATIC
  src/tilde.cpp)
add_library(gnash::tilde ALIAS gnash_tilde)

target_link_libraries(gnash_tilde
  PUBLIC  gnash_common
  PRIVATE gnash::sh gnash_warnings)
