# The GNU History reimplementation.
#
# Public C++ header:  <gnash/history.hpp>
# Drop-in C shim:     <readline/history.h>  (implemented in src/shim.cpp)
add_library(gnash_history STATIC
  src/history.cpp
  src/histfile.cpp
  src/histexpand.cpp
  src/shim.cpp)
add_library(gnash::history ALIAS gnash_history)

target_link_libraries(gnash_history
  PUBLIC  gnash_common
  PRIVATE gnash::sh gnash_warnings)
