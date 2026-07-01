# The shell core (in progress): lexer, parser, AST, expansion, execution.
add_library(gnash_core STATIC
  src/ast.cpp
  src/lexer.cpp
  src/parser.cpp
  src/shell.cpp
  src/arith.cpp
  src/expand.cpp
  src/executor.cpp
  src/builtins.cpp
  src/jobs.cpp
  src/prompt.cpp
  src/repl.cpp)
add_library(gnash::core ALIAS gnash_core)

target_include_directories(gnash_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(gnash_core
  PUBLIC  gnash_common
  PRIVATE gnash::glob gnash::readline gnash::history gnash_warnings)

# gnash-parse: syntax-check / dump the AST (accept/reject like `bash -n`).
add_executable(gnash-parse tools/gnash_parse.cpp)
target_link_libraries(gnash-parse PRIVATE gnash::core)

# gnash: the shell itself.
add_executable(gnash src/main.cpp)
target_link_libraries(gnash PRIVATE gnash::core)
