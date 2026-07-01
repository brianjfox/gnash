add_library(gnash_sh STATIC
  src/xmalloc.cpp
  src/quote.cpp
  src/shellenv.cpp)
add_library(gnash::sh ALIAS gnash_sh)

target_include_directories(gnash_sh PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(gnash_sh
  PUBLIC  gnash_common
  PRIVATE gnash_warnings)
