// Copyright (c) 2026 Brian J. Fox
// Licensed under GPLv2 with the GPLv2-AI Exception.

// shim.cpp -- the drop-in C entry points declared in <readline/history.h>.
//
// Each function forwards to the process-global History returned by
// default_history(), keeping the classic history_* globals synchronised so
// code that reads them directly (as bash does) sees consistent values.

#include <cstdlib>
#include <cstring>
#include <vector>

#include "gnash/history.hpp"
#include "gnash/sh/xmalloc.hpp"
#include "readline/history.h"

using gnash::history::Entry;
using gnash::history::History;
using gnash::history::StateSnapshot;
using gnash::history::default_history;

// ---- exported globals (mirror lib/readline/history.c) ---------------------
extern "C" {
int history_base = 1;
int history_length = 0;
int history_max_entries = 0;
int history_offset = 0;
int history_lines_read_from_file = 0;
int history_lines_written_to_file = 0;
char history_comment_char = '#';
int history_write_timestamps = 0;
int max_input_history = 0;
}

namespace {

// Push caller-set tunables into the instance and return it.
History &H() {
  History &h = default_history();
  h.comment_char = history_comment_char;
  h.write_timestamps = history_write_timestamps != 0;
  return h;
}

// Pull instance state back out into the exported globals.
void sync_out(const History &h) {
  history_base = h.base();
  history_length = h.length();
  history_offset = h.offset();
  history_max_entries = h.max_entries();
  max_input_history = history_max_entries;
  history_lines_read_from_file = h.lines_read_from_file;
  history_lines_written_to_file = h.lines_written_to_file;
}

}  // namespace

extern "C" {

void using_history(void) {
  History &h = H();
  h.begin_session();
  sync_out(h);
}

void add_history(const char *string) {
  History &h = H();
  h.add(string ? string : "");
  sync_out(h);
}

void add_history_time(const char *string) {
  History &h = H();
  if (string) h.add_time(string);
  sync_out(h);
}

HIST_ENTRY *remove_history(int which) {
  History &h = H();
  Entry *e = h.remove(which);
  sync_out(h);
  return e;
}

HIST_ENTRY **remove_history_range(int first, int last) {
  History &h = H();
  std::vector<Entry *> removed = h.remove_range(first, last);
  sync_out(h);
  if (removed.empty()) return nullptr;
  HIST_ENTRY **ret = static_cast<HIST_ENTRY **>(
      gnash::sh::xmalloc((removed.size() + 1) * sizeof(HIST_ENTRY *)));
  std::size_t i = 0;
  for (; i < removed.size(); i++) ret[i] = removed[i];
  ret[i] = nullptr;
  return ret;
}

HIST_ENTRY *alloc_history_entry(char *string, char *ts) {
  return History::alloc_entry(string, ts);
}

HIST_ENTRY *copy_history_entry(HIST_ENTRY *entry) {
  return History::copy_entry(entry);
}

histdata_t free_history_entry(HIST_ENTRY *entry) {
  return History::free_entry(entry);
}

HIST_ENTRY *replace_history_entry(int which, const char *line, histdata_t data) {
  History &h = H();
  Entry *old = h.replace(which, line ? line : "", data);
  sync_out(h);
  return old;
}

void clear_history(void) {
  History &h = H();
  h.clear();
  sync_out(h);
}

void stifle_history(int max) {
  History &h = H();
  h.stifle(max);
  sync_out(h);
}

int unstifle_history(void) {
  History &h = H();
  int r = h.unstifle();
  sync_out(h);
  return r;
}

int history_is_stifled(void) { return default_history().is_stifled() ? 1 : 0; }

HIST_ENTRY **history_list(void) {
  return const_cast<HIST_ENTRY **>(H().list());
}

int where_history(void) { return default_history().offset(); }

HIST_ENTRY *current_history(void) { return H().current(); }

HIST_ENTRY *history_get(int offset) { return H().get(offset); }

time_t history_get_time(HIST_ENTRY *entry) {
  return default_history().entry_time(entry);
}

int history_total_bytes(void) { return default_history().total_bytes(); }

int history_set_pos(int pos) {
  History &h = H();
  int r = h.set_pos(pos) ? 1 : 0;
  sync_out(h);
  return r;
}

HIST_ENTRY *previous_history(void) {
  History &h = H();
  Entry *e = h.previous();
  sync_out(h);
  return e;
}

HIST_ENTRY *next_history(void) {
  History &h = H();
  Entry *e = h.next();
  sync_out(h);
  return e;
}

int history_search(const char *string, int direction) {
  History &h = H();
  int r = h.search(string ? string : "", direction);
  sync_out(h);
  return r;
}

int history_search_prefix(const char *string, int direction) {
  History &h = H();
  int r = h.search_prefix(string ? string : "", direction);
  sync_out(h);
  return r;
}

int history_search_pos(const char *string, int direction, int pos) {
  History &h = H();
  int r = h.search_pos(string ? string : "", direction, pos);
  sync_out(h);
  return r;
}

int read_history(const char *filename) {
  History &h = H();
  int r = h.read_file(filename);
  sync_out(h);
  return r;
}

int read_history_range(const char *filename, int from, int to) {
  History &h = H();
  int r = h.read_file_range(filename, from, to);
  sync_out(h);
  return r;
}

int write_history(const char *filename) {
  History &h = H();
  int r = h.write_file(filename);
  sync_out(h);
  return r;
}

int append_history(int nelement, const char *filename) {
  History &h = H();
  int r = h.append_file(nelement, filename);
  sync_out(h);
  return r;
}

int history_truncate_file(const char *filename, int nlines) {
  return H().truncate_file(filename, nlines);
}

HISTORY_STATE *history_get_history_state(void) {
  History &h = H();
  StateSnapshot s = h.state();
  HISTORY_STATE *st = static_cast<HISTORY_STATE *>(
      gnash::sh::xmalloc(sizeof(HISTORY_STATE)));
  st->entries = static_cast<HIST_ENTRY **>(
      gnash::sh::xmalloc((s.entries.size() + 1) * sizeof(HIST_ENTRY *)));
  std::size_t i = 0;
  for (; i < s.entries.size(); i++) st->entries[i] = s.entries[i];
  st->entries[i] = nullptr;
  st->offset = s.offset;
  st->length = s.length;
  st->size = s.length;
  st->flags = s.stifled ? HS_STIFLED : 0;
  return st;
}

void history_set_history_state(HISTORY_STATE *st) {
  if (st == nullptr) return;
  History &h = H();
  StateSnapshot s;
  for (int i = 0; st->entries && st->entries[i]; i++)
    s.entries.push_back(st->entries[i]);
  s.offset = st->offset;
  s.length = static_cast<int>(s.entries.size());
  s.base = 1;
  s.stifled = (st->flags & HS_STIFLED) != 0;
  s.max_entries = history_max_entries;
  h.set_state(s);
  sync_out(h);
}

// history_expand() and its companions (get_history_event, history_arg_extract,
// history_tokenize) live in histexpand.cpp.

}  // extern "C"
