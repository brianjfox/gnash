// Copyright (c) 2026 Brian J. Fox
// Licensed under GPLv2 with the GPLv2-AI Exception.

// history_test.cpp -- unit/smoke tests for gnash's History library.
//
// Covers list management, navigation, search, stifling, and a file save/load
// round-trip through both the modern C++ API and the drop-in C shim.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "gnash/history.hpp"
#include "readline/history.h"

namespace hist = gnash::history;

static int failures = 0;

#define CHECK(cond)                                                       \
  do {                                                                    \
    if (!(cond)) {                                                        \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      failures++;                                                         \
    }                                                                     \
  } while (0)

static std::string tmp_path(const char *name) {
  const char *dir = std::getenv("TMPDIR");
  if (dir == nullptr || *dir == '\0') dir = "/tmp";
  return std::string(dir) + "/" + name;
}

static void test_add_and_access() {
  hist::History h;
  h.add("one");
  h.add("two");
  h.add("three");

  CHECK(h.length() == 3);
  CHECK(h.base() == 1);
  CHECK(std::strcmp(h.get(1)->line, "one") == 0);    // logical, base-relative
  CHECK(std::strcmp(h.get(3)->line, "three") == 0);
  CHECK(h.get(4) == nullptr);

  hist::Entry *const *list = h.list();
  CHECK(list[0] && std::strcmp(list[0]->line, "one") == 0);
  CHECK(list[3] == nullptr);
}

static void test_navigation() {
  hist::History h;
  h.add("a");
  h.add("b");
  h.add("c");
  h.begin_session();
  CHECK(h.offset() == 3);
  CHECK(h.current() == nullptr);           // at end
  CHECK(std::strcmp(h.previous()->line, "c") == 0);
  CHECK(std::strcmp(h.previous()->line, "b") == 0);
  CHECK(std::strcmp(h.current()->line, "b") == 0);
  CHECK(std::strcmp(h.next()->line, "c") == 0);
  CHECK(h.next() == nullptr);              // stepped to the end
  CHECK(h.previous() && std::strcmp(h.current()->line, "c") == 0);
}

static void test_search() {
  hist::History h;
  h.add("apple pie");
  h.add("banana bread");
  h.add("cherry apple");
  h.begin_session();

  // Backward substring search from the end.
  int pos = h.search("apple", hist::kBackward);
  CHECK(pos >= 0);
  CHECK(std::strcmp(h.current()->line, "cherry apple") == 0);

  // Anchored (prefix) search.
  h.begin_session();
  int p2 = h.search_prefix("banana", hist::kBackward);
  CHECK(p2 == 0);
  CHECK(std::strcmp(h.current()->line, "banana bread") == 0);

  // Absolute-position search leaves the offset unchanged.
  int before = h.offset();
  int idx = h.search_pos("apple", hist::kForward, 0);
  CHECK(idx == 0);  // "apple pie" is entry 0
  CHECK(h.offset() == before);
}

static void test_stifle() {
  hist::History h;
  for (int i = 0; i < 10; i++) h.add(std::to_string(i));
  CHECK(h.length() == 10);

  h.stifle(4);
  CHECK(h.is_stifled());
  CHECK(h.length() == 4);
  CHECK(h.base() == 6);  // len(10) - max(4), matching history.c
  CHECK(std::strcmp(h.list()[0]->line, "6") == 0);

  // Once stifled at 4, adding keeps only the last 4 and advances base.
  h.add("10");
  CHECK(h.length() == 4);
  CHECK(std::strcmp(h.list()[0]->line, "7") == 0);
  CHECK(std::strcmp(h.list()[3]->line, "10") == 0);
  CHECK(h.base() == 7);

  int prev_max = h.unstifle();
  CHECK(prev_max == 4);
  CHECK(!h.is_stifled());
}

static void test_remove_replace() {
  hist::History h;
  h.add("x");
  h.add("y");
  h.add("z");

  hist::Entry *removed = h.remove(1);  // "y"
  CHECK(removed && std::strcmp(removed->line, "y") == 0);
  hist::History::free_entry(removed);
  CHECK(h.length() == 2);
  CHECK(std::strcmp(h.list()[1]->line, "z") == 0);

  hist::Entry *old = h.replace(0, "X", nullptr);
  CHECK(old && std::strcmp(old->line, "x") == 0);
  hist::History::free_entry(old);
  CHECK(std::strcmp(h.list()[0]->line, "X") == 0);
}

static void test_file_roundtrip() {
  std::string path = tmp_path("gnash_hist_roundtrip.tmp");

  hist::History h;
  h.add("first line");
  h.add("second line");
  h.add("third line");
  CHECK(h.write_file(path.c_str()) == 0);

  hist::History h2;
  CHECK(h2.read_file(path.c_str()) == 0);
  CHECK(h2.length() == 3);
  CHECK(std::strcmp(h2.get(1)->line, "first line") == 0);
  CHECK(std::strcmp(h2.get(3)->line, "third line") == 0);

  std::remove(path.c_str());
}

static void test_file_timestamps() {
  std::string path = tmp_path("gnash_hist_ts.tmp");

  hist::History h;
  h.write_timestamps = true;
  h.add("cmd one");
  h.add("cmd two");
  CHECK(h.write_file(path.c_str()) == 0);

  hist::History h2;
  CHECK(h2.read_file(path.c_str()) == 0);
  CHECK(h2.length() == 2);
  // Timestamp lines were re-attached to the following entries.
  CHECK(h2.entry_time(h2.get(1)) > 0);
  CHECK(h2.entry_time(h2.get(2)) > 0);

  std::remove(path.c_str());
}

static void test_c_shim() {
  clear_history();  // reset the process-global instance
  CHECK(history_length == 0);

  add_history("alpha");
  add_history("beta");
  CHECK(history_length == 2);
  CHECK(history_base == 1);

  HIST_ENTRY *e = history_get(2);
  CHECK(e && std::strcmp(e->line, "beta") == 0);

  using_history();
  CHECK(where_history() == 2);
  HIST_ENTRY *p = previous_history();
  CHECK(p && std::strcmp(p->line, "beta") == 0);

  stifle_history(1);
  CHECK(history_is_stifled() == 1);
  CHECK(history_length == 1);
  CHECK(std::strcmp(history_list()[0]->line, "beta") == 0);

  clear_history();
}

int main() {
  test_add_and_access();
  test_navigation();
  test_search();
  test_stifle();
  test_remove_replace();
  test_file_roundtrip();
  test_file_timestamps();
  test_c_shim();

  if (failures == 0) {
    std::printf("all history tests passed\n");
    return 0;
  }
  std::fprintf(stderr, "%d history test(s) failed\n", failures);
  return 1;
}
