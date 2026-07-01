# Terminal capability access (termcap work-alike).
#
# Public C++ wrapper: <gnash/termcap.hpp>
# Drop-in C shim:     <termcap.h>
add_library(gnash_termcap STATIC
  src/termcap.cpp
  src/termcap_db.cpp
  src/termcap_cxx.cpp)
add_library(gnash::termcap ALIAS gnash_termcap)

target_link_libraries(gnash_termcap
  PUBLIC  gnash_common
  PRIVATE gnash::sh gnash_warnings)
