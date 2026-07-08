# Copyright (c) 2026 Brian J. Fox
# Licensed under GPLv2 with the GPLv2-AI Exception.

cmake_minimum_required(VERSION 3.16)

project(gnash
  VERSION 1.3.4
  DESCRIPTION "A modular C++ reimplementation of bash 5.3"
  LANGUAGES C CXX)

# ---------------------------------------------------------------------------
# Language standard
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(GnashWarnings)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(GNASH_BUILD_TESTS "Build gnash tests" ON)
option(GNASH_WERROR      "Treat warnings as errors" OFF)
option(GNASH_SANITIZE    "Enable ASan/UBSan" OFF)

if(GNASH_SANITIZE)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

# ---------------------------------------------------------------------------
# Shared include roots.
#
#   include/  -> modern C++ public headers          (namespace gnash::*)
#   compat/   -> C-compatible "shim" headers         (<readline/history.h> ...)
#
# gnash_common carries these to every target so the dependency graph is
# expressed purely through target_link_libraries().
# ---------------------------------------------------------------------------
add_library(gnash_common INTERFACE)
target_include_directories(gnash_common INTERFACE
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${CMAKE_CURRENT_SOURCE_DIR}/compat)
target_compile_features(gnash_common INTERFACE cxx_std_20)
# Single source of truth for the gnash version (from project() above), available
# to the code as the GNASH_VERSION string macro.
target_compile_definitions(gnash_common INTERFACE GNASH_VERSION="${PROJECT_VERSION}")

# ---------------------------------------------------------------------------
# Libraries, lowest-dependency-first.  See the approved plan for the full
# layering: libsh -> {tilde,termcap} -> history -> readline -> glob -> core.
# ---------------------------------------------------------------------------
add_subdirectory(libsh)
add_subdirectory(libtilde)
add_subdirectory(libtermcap)
add_subdirectory(libhistory)
add_subdirectory(libreadline)
add_subdirectory(libglob)
add_subdirectory(core)

if(GNASH_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
