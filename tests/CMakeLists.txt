# Copyright (c) 2026 Brian J. Fox
# Licensed under GPLv2 with the GPLv2-AI Exception.

# ---------------------------------------------------------------------------
# Unit tests
# ---------------------------------------------------------------------------
add_executable(history_test history_test.cpp)
target_link_libraries(history_test PRIVATE gnash::history gnash::sh gnash_warnings)
add_test(NAME history_test COMMAND history_test)

add_executable(histexpand_test histexpand_test.cpp)
target_link_libraries(histexpand_test PRIVATE gnash::history gnash::sh gnash_warnings)
add_test(NAME histexpand_test COMMAND histexpand_test)

add_executable(tilde_test tilde_test.cpp)
target_link_libraries(tilde_test PRIVATE gnash::tilde gnash::sh gnash_warnings)
add_test(NAME tilde_test COMMAND tilde_test)

add_executable(termcap_test termcap_test.cpp)
target_link_libraries(termcap_test PRIVATE gnash::termcap gnash::sh gnash_warnings)
add_test(NAME termcap_test COMMAND termcap_test)

add_executable(readline_edit_test readline_edit_test.cpp)
target_link_libraries(readline_edit_test PRIVATE gnash::readline gnash_warnings)
add_test(NAME readline_edit_test COMMAND readline_edit_test)

add_executable(readline_dispatch_test readline_dispatch_test.cpp)
target_link_libraries(readline_dispatch_test PRIVATE gnash::readline gnash::history gnash_warnings)
add_test(NAME readline_dispatch_test COMMAND readline_dispatch_test)

add_executable(readline_complete_test readline_complete_test.cpp)
target_link_libraries(readline_complete_test PRIVATE gnash::readline gnash::history gnash_warnings)
add_test(NAME readline_complete_test COMMAND readline_complete_test)

add_executable(readline_bind_test readline_bind_test.cpp)
target_link_libraries(readline_bind_test PRIVATE gnash::readline gnash_warnings)
add_test(NAME readline_bind_test COMMAND readline_bind_test)

add_executable(glob_test glob_test.cpp)
target_link_libraries(glob_test PRIVATE gnash::glob gnash_warnings)
add_test(NAME glob_test COMMAND glob_test)

add_executable(lexer_test lexer_test.cpp)
target_link_libraries(lexer_test PRIVATE gnash::core gnash_warnings)
add_test(NAME lexer_test COMMAND lexer_test)

add_executable(parser_test parser_test.cpp)
target_link_libraries(parser_test PRIVATE gnash::core gnash_warnings)
add_test(NAME parser_test COMMAND parser_test)

# Differential parser check vs `bash -n` over the supported-subset corpus.
if(BASH_EXECUTABLE)
  add_test(
    NAME parse_diff
    COMMAND ${BASH_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/harness/parse_diff.sh
            $<TARGET_FILE:gnash-parse> ${BASH_EXECUTABLE})
endif()

add_executable(arith_test arith_test.cpp)
target_link_libraries(arith_test PRIVATE gnash::core gnash_warnings)
add_test(NAME arith_test COMMAND arith_test)

# Differential execution check vs bash over the implemented feature set.
if(BASH_EXECUTABLE)
  add_test(
    NAME run_diff
    COMMAND ${BASH_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/harness/run_diff.sh
            $<TARGET_FILE:gnash> ${BASH_EXECUTABLE})

  # Error-message format check vs bash (program name normalized).
  add_test(
    NAME errfmt
    COMMAND ${BASH_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/harness/errfmt.sh
            $<TARGET_FILE:gnash> ${BASH_EXECUTABLE})

  # Regression gate over the bash tests/ files gnash passes byte-for-byte.
  add_test(
    NAME conformance_gate
    COMMAND ${BASH_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/harness/conformance_gate.sh
            $<TARGET_FILE:gnash> ${CMAKE_CURRENT_SOURCE_DIR}/bash-tests)
endif()

# Differential execution check for the csh personality vs a reference tcsh.
find_program(TCSH_EXECUTABLE tcsh)
if(BASH_EXECUTABLE AND TCSH_EXECUTABLE)
  add_test(
    NAME run_diff_csh
    COMMAND ${BASH_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/harness/run_diff_csh.sh
            $<TARGET_FILE:gnash> ${TCSH_EXECUTABLE})
endif()

# Differential execution check for the zsh personality vs a reference zsh,
# covering the expansion behaviors (word splitting, bare-array expansion) where
# zsh diverges from bash.
find_program(ZSH_EXECUTABLE zsh)
if(BASH_EXECUTABLE AND ZSH_EXECUTABLE)
  add_test(
    NAME run_diff_zsh
    COMMAND ${BASH_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/harness/run_diff_zsh.sh
            $<TARGET_FILE:gnash> ${ZSH_EXECUTABLE})
endif()

# Differential execution check for the ksh personality vs a reference ksh93,
# over common ksh/POSIX constructs on the shared Bourne engine.
find_program(KSH_EXECUTABLE ksh)
if(BASH_EXECUTABLE AND KSH_EXECUTABLE)
  add_test(
    NAME run_diff_ksh
    COMMAND ${BASH_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/harness/run_diff_ksh.sh
            $<TARGET_FILE:gnash> ${KSH_EXECUTABLE})
endif()

# Behavioral check for the runtime personality/emulate builtin.
if(BASH_EXECUTABLE)
  add_test(
    NAME personality_test
    COMMAND ${BASH_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/harness/personality_test.sh
            $<TARGET_FILE:gnash>)
endif()

# ---------------------------------------------------------------------------
# Conformance harness self-check.
#
# The harness (harness/run_test.sh) runs a bash-style *.tests file under a
# given shell and diffs the combined output against the matching *.right file.
# Until the gnash executable exists we validate the harness itself against the
# system bash, proving it consumes bash's test format correctly.
# ---------------------------------------------------------------------------
find_program(BASH_EXECUTABLE bash)
if(BASH_EXECUTABLE)
  add_test(
    NAME harness_selfcheck
    COMMAND ${BASH_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/harness/run_test.sh
            ${BASH_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/smoke.tests
            ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/smoke.right)
endif()
