# Copyright (c) 2026 Brian J. Fox
# Licensed under GPLv2 with the GPLv2-AI Exception.

# The Readline reimplementation (in progress).
#
# Public C++ surface: <gnash/readline.hpp>
# Drop-in C shim:     <readline/readline.h>, <readline/keymaps.h>
#
# Present: editing core, keymap-driven dispatch, tty raw mode, single-line
# redisplay, and the interactive readline() entry point.  Next: completion
# hooks, incremental search, vi mode, .inputrc.
add_library(gnash_readline STATIC
  src/state.cpp
  src/text.cpp
  src/keymaps.cpp
  src/tty.cpp
  src/input.cpp
  src/readline.cpp
  src/complete.cpp
  src/isearch.cpp
  src/vi_mode.cpp
  src/bind.cpp
  src/readline_cxx.cpp)
add_library(gnash::readline ALIAS gnash_readline)

target_include_directories(gnash_readline PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(gnash_readline
  PUBLIC  gnash_common
  PRIVATE gnash::sh gnash::history gnash::termcap gnash::tilde gnash_warnings)

# Interactive demo: a tiny REPL that echoes each line and records history.
add_executable(gnash_rl_demo examples/gnash_rl_demo.cpp)
target_link_libraries(gnash_rl_demo PRIVATE gnash::readline gnash::history)
