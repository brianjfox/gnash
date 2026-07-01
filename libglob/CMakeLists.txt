# Globbing and pattern matching.
#
# Public C++ surface: <gnash/glob.hpp>
# Drop-in C shims:    <strmatch.h> (strmatch), <glob.h> (glob_filename)
add_library(gnash_glob STATIC
  src/smatch.cpp
  src/glob.cpp)
add_library(gnash::glob ALIAS gnash_glob)

target_link_libraries(gnash_glob
  PUBLIC  gnash_common
  PRIVATE gnash::sh gnash_warnings)
