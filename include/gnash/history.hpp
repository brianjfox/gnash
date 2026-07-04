// Copyright (c) 2026 Brian J. Fox
// Licensed under GPLv2 with the GPLv2-AI Exception.

// history.hpp -- the modern C++ interface to gnash's GNU History reimplementation.
//
// This is the *real* interface.  The classic C entry points (add_history(),
// read_history(), history_expand(), the history_* globals, ...) live in
// <readline/history.h> and are a thin shim over a process-global History
// instance returned by default_history().
//
// Behaviour is derived from bash 5.3's lib/readline/history.c, histfile.c,
// histsearch.c and histexpand.c and is intended to match them exactly.
#ifndef GNASH_HISTORY_HPP
#define GNASH_HISTORY_HPP

#include <ctime>
#include <deque>
#include <string>
#include <string_view>
#include <vector>

#include "gnash/hist_entry.h"

namespace gnash::history {

using Entry = ::gnash_hist_entry;
using histdata_t = void *;

// Snapshot of the interactive history variables, analogous to HISTORY_STATE.
struct StateSnapshot {
  std::vector<Entry *> entries;  // borrowed pointers, [0, length)
  int offset = 0;
  int length = 0;
  int base = 1;
  bool stifled = false;
  int max_entries = 0;
};

// Direction constants for the search family (matching History's convention).
inline constexpr int kBackward = -1;
inline constexpr int kForward = 1;

class History {
 public:
  History();
  ~History();
  History(const History &) = delete;
  History &operator=(const History &) = delete;

  // -- session ------------------------------------------------------------
  // Reset the interactive pointer to the end of the list (using_history()).
  void begin_session();

  // -- list management ----------------------------------------------------
  void add(std::string_view line);           // add_history
  void add_time(std::string_view ts);        // add_history_time (most recent)
  Entry *remove(int which);                   // remove_history (ownership out)
  std::vector<Entry *> remove_range(int first, int last);  // remove_history_range
  Entry *replace(int which, std::string_view line, histdata_t data);
  void clear();                               // clear_history
  void stifle(int max);                       // stifle_history
  int unstifle();                             // unstifle_history
  bool is_stifled() const { return stifled_; }

  // -- information --------------------------------------------------------
  int length() const { return static_cast<int>(entries_.size()); }
  int base() const { return base_; }
  int offset() const { return offset_; }      // where_history
  void set_offset(int o) { offset_ = o; }      // raw set (history_offset = o)
  int max_entries() const { return max_entries_; }
  bool at_end() const { return entries_.empty() || offset_ == length(); }
  int total_bytes() const;                    // history_total_bytes
  std::time_t entry_time(const Entry *e) const;  // history_get_time

  // NULL-terminated view of the current list (history_list()).  Valid until
  // the next mutating call.
  Entry *const *list();

  // -- navigation ---------------------------------------------------------
  bool set_pos(int pos);        // history_set_pos
  Entry *current();             // current_history
  Entry *previous();            // previous_history
  Entry *next();                // next_history
  Entry *get(int logical_offset);  // history_get (relative to base)

  // -- searching ----------------------------------------------------------
  int search(std::string_view s, int direction);         // history_search
  int search_prefix(std::string_view s, int direction);  // history_search_prefix
  int search_pos(std::string_view s, int direction, int pos);  // history_search_pos

  // -- state --------------------------------------------------------------
  StateSnapshot state() const;                 // history_get_history_state
  void set_state(const StateSnapshot &s);       // history_set_history_state

  // -- file I/O (return 0 on success, else errno) -------------------------
  int read_file(const char *filename);                       // read_history
  int read_file_range(const char *filename, int from, int to);  // read_history_range
  int write_file(const char *filename);                      // write_history
  int append_file(int nelement, const char *filename);       // append_history
  int truncate_file(const char *filename, int nlines);       // history_truncate_file

  // -- entry helpers (static; mirror the classic allocators) --------------
  static Entry *alloc_entry(const char *line, char *timestamp /*owned*/);
  static Entry *copy_entry(const Entry *e);
  static histdata_t free_entry(Entry *e);      // returns the data field

  // -- tunables (mirror the exported history_* globals) -------------------
  char comment_char = '#';
  bool write_timestamps = false;
  int lines_read_from_file = 0;
  int lines_written_to_file = 0;

 private:
  std::string init_time() const;
  int search_internal(std::string_view s, int direction, bool anchored);

  std::deque<Entry *> entries_;
  std::vector<Entry *> view_;   // scratch for list()
  int offset_ = 0;              // history_offset
  int base_ = 1;               // history_base
  bool stifled_ = false;       // history_stifled
  int max_entries_ = 0;        // history_max_entries
};

// The process-global history used by the C shim and convenience wrappers.
History &default_history();

}  // namespace gnash::history

#endif  // GNASH_HISTORY_HPP
