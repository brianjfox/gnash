// Copyright (c) 2026 Brian J. Fox
// Licensed under GPLv2 with the GPLv2-AI Exception.

// history.cpp -- history list management, navigation, and search.
//
// Faithful reimplementation of bash 5.3 lib/readline/history.c and
// histsearch.c.  The observable state (length, base, offset, stifling, entry
// order and contents) matches; the internal storage uses a std::deque of
// owned entries instead of the roving real_history/the_history window, which
// is an implementation detail with no user-visible effect.

#include "gnash/history.hpp"

#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <string>

#include "gnash/sh/xmalloc.hpp"

namespace gnash::history {

using gnash::sh::savestring;
using gnash::sh::xfree;
using gnash::sh::xmalloc;

// --------------------------------------------------------------------------
// Entry helpers
// --------------------------------------------------------------------------

Entry *History::alloc_entry(const char *line, char *timestamp) {
  Entry *e = static_cast<Entry *>(xmalloc(sizeof(Entry)));
  e->line = line ? savestring(line) : nullptr;
  e->timestamp = timestamp;  // takes ownership
  e->data = nullptr;
  return e;
}

Entry *History::copy_entry(const Entry *e) {
  if (e == nullptr) return nullptr;
  Entry *ret = alloc_entry(e->line, nullptr);
  ret->timestamp = e->timestamp ? savestring(e->timestamp) : nullptr;
  ret->data = e->data;
  return ret;
}

histdata_t History::free_entry(Entry *e) {
  if (e == nullptr) return nullptr;
  xfree(e->line);
  xfree(e->timestamp);
  histdata_t d = e->data;
  xfree(e);
  return d;
}

// --------------------------------------------------------------------------
// Construction / lifetime
// --------------------------------------------------------------------------

History::History() = default;

History::~History() {
  for (Entry *e : entries_) free_entry(e);
}

std::string History::init_time() const {
  std::string ts;
  ts.push_back(comment_char);
  ts += std::to_string(static_cast<long long>(std::time(nullptr)));
  return ts;
}

void History::begin_session() { offset_ = length(); }

// --------------------------------------------------------------------------
// List management
// --------------------------------------------------------------------------

void History::add(std::string_view line) {
  if (stifled_ && length() == max_entries_) {
    // Stifled and full: drop the oldest and advance the logical base.
    if (entries_.empty())  // max_entries_ == 0: never store anything
      return;
    free_entry(entries_.front());
    entries_.pop_front();
    base_++;
  }

  std::string tmp(line);
  std::string ts = init_time();
  Entry *e = alloc_entry(tmp.c_str(), savestring(ts.c_str()));
  entries_.push_back(e);
}

void History::add_time(std::string_view ts) {
  if (ts.empty() || entries_.empty()) return;
  Entry *e = entries_.back();
  xfree(e->timestamp);
  std::string tmp(ts);
  e->timestamp = savestring(tmp.c_str());
}

Entry *History::remove(int which) {
  if (which < 0 || which >= length() || entries_.empty())
    return nullptr;
  Entry *ret = entries_[static_cast<std::size_t>(which)];
  entries_.erase(entries_.begin() + which);
  return ret;
}

std::vector<Entry *> History::remove_range(int first, int last) {
  std::vector<Entry *> ret;
  if (entries_.empty()) return ret;
  if (first < 0 || first >= length() || last < 0 || last >= length())
    return ret;
  if (first > last) return ret;
  for (int i = first; i <= last; i++)
    ret.push_back(entries_[static_cast<std::size_t>(i)]);
  entries_.erase(entries_.begin() + first, entries_.begin() + last + 1);
  return ret;
}

Entry *History::replace(int which, std::string_view line, histdata_t data) {
  if (which < 0 || which >= length()) return nullptr;
  Entry *old = entries_[static_cast<std::size_t>(which)];
  std::string tmp(line);
  Entry *e = static_cast<Entry *>(xmalloc(sizeof(Entry)));
  e->line = savestring(tmp.c_str());
  e->data = data;
  e->timestamp = old->timestamp ? savestring(old->timestamp) : nullptr;
  entries_[static_cast<std::size_t>(which)] = e;
  return old;
}

void History::clear() {
  for (Entry *e : entries_) free_entry(e);
  entries_.clear();
  offset_ = 0;
  base_ = 1;  // reset to default
}

void History::stifle(int max) {
  if (max < 0) max = 0;
  int len = length();
  if (len > max) {
    for (int i = 0; i < len - max; i++) {
      free_entry(entries_.front());
      entries_.pop_front();
    }
    base_ = len - max;  // matches history.c exactly
  }
  stifled_ = true;
  max_entries_ = max;
}

int History::unstifle() {
  if (stifled_) {
    stifled_ = false;
    return max_entries_;
  }
  return -max_entries_;
}

// --------------------------------------------------------------------------
// Information
// --------------------------------------------------------------------------

int History::total_bytes() const {
  int result = 0;
  for (const Entry *e : entries_) {
    if (e->line) result += static_cast<int>(std::strlen(e->line));
    if (e->timestamp) result += static_cast<int>(std::strlen(e->timestamp));
  }
  return result;
}

std::time_t History::entry_time(const Entry *e) const {
  if (e == nullptr || e->timestamp == nullptr) return 0;
  if (e->timestamp[0] != comment_char) return 0;
  errno = 0;
  std::time_t t = static_cast<std::time_t>(std::strtol(e->timestamp + 1, nullptr, 10));
  if (errno == ERANGE) return 0;
  return t;
}

Entry *const *History::list() {
  view_.clear();
  view_.reserve(entries_.size() + 1);
  for (Entry *e : entries_) view_.push_back(e);
  view_.push_back(nullptr);
  return view_.data();
}

// --------------------------------------------------------------------------
// Navigation
// --------------------------------------------------------------------------

bool History::set_pos(int pos) {
  if (pos > length() || pos < 0 || entries_.empty()) return false;
  offset_ = pos;
  return true;
}

Entry *History::current() {
  if (offset_ == length() || entries_.empty()) return nullptr;
  return entries_[static_cast<std::size_t>(offset_)];
}

Entry *History::previous() {
  if (offset_ == 0) return nullptr;
  return entries_[static_cast<std::size_t>(--offset_)];
}

Entry *History::next() {
  if (offset_ == length()) return nullptr;
  ++offset_;
  if (offset_ == length()) return nullptr;  // stepped onto the NULL terminator
  return entries_[static_cast<std::size_t>(offset_)];
}

Entry *History::get(int logical_offset) {
  int local = logical_offset - base_;
  if (local >= length() || local < 0 || entries_.empty()) return nullptr;
  return entries_[static_cast<std::size_t>(local)];
}

// --------------------------------------------------------------------------
// Searching (histsearch.c)
// --------------------------------------------------------------------------

// Faithful to histsearch.c history_search_internal with listdir == linedir
// (the convention used by history_search / history_search_prefix).
int History::search_internal(std::string_view s, int direction, bool anchored) {
  const int slen = static_cast<int>(s.size());
  const bool reverse = direction < 0;

  if (slen == 0) return -1;
  if (length() == 0 || (offset_ >= length() && !reverse)) return -1;

  int i = offset_;
  if (reverse && i >= length()) i = length() - 1;

  auto next_line = [&]() { i += reverse ? -1 : 1; };

  for (;;) {
    if ((reverse && i < 0) || (!reverse && i == length())) return -1;

    const char *line = entries_[static_cast<std::size_t>(i)]->line;
    const int llen = line ? static_cast<int>(std::strlen(line)) : 0;

    if (slen > llen) {
      next_line();
      continue;
    }

    if (anchored) {
      if (std::strncmp(line, s.data(), static_cast<std::size_t>(slen)) == 0) {
        offset_ = i;
        return 0;
      }
      next_line();
      continue;
    }

    // Non-anchored substring search; line direction follows list direction.
    if (reverse) {  // search backwards from the end of the line
      for (int idx = llen - slen; idx >= 0; idx--) {
        if (std::strncmp(line + idx, s.data(), static_cast<std::size_t>(slen)) == 0) {
          offset_ = i;
          return idx;
        }
      }
    } else {  // search forwards from the start of the line
      int limit = llen - slen + 1;
      for (int idx = 0; idx < limit; idx++) {
        if (std::strncmp(line + idx, s.data(), static_cast<std::size_t>(slen)) == 0) {
          offset_ = i;
          return idx;
        }
      }
    }

    next_line();
  }
}

int History::search(std::string_view s, int direction) {
  return search_internal(s, direction, /*anchored=*/false);
}

int History::search_prefix(std::string_view s, int direction) {
  return search_internal(s, direction, /*anchored=*/true);
}

int History::search_pos(std::string_view s, int direction, int pos) {
  int old = offset_;
  if (!set_pos(pos)) {
    // history_set_pos failed; fall back to leaving offset where it was.
  }
  int r = search(s, direction);
  if (r == -1) {
    offset_ = old;
    return -1;
  }
  int found = offset_;
  offset_ = old;
  return found;
}

// --------------------------------------------------------------------------
// State
// --------------------------------------------------------------------------

StateSnapshot History::state() const {
  StateSnapshot s;
  s.entries.assign(entries_.begin(), entries_.end());
  s.offset = offset_;
  s.length = length();
  s.base = base_;
  s.stifled = stifled_;
  s.max_entries = max_entries_;
  return s;
}

void History::set_state(const StateSnapshot &s) {
  // Replace our list wholesale, taking ownership of the provided entries.
  for (Entry *e : entries_) free_entry(e);
  entries_.assign(s.entries.begin(), s.entries.end());
  offset_ = s.offset;
  base_ = s.base;
  stifled_ = s.stifled;
  max_entries_ = s.max_entries;
}

// --------------------------------------------------------------------------
// Process-global instance
// --------------------------------------------------------------------------

History &default_history() {
  static History instance;
  return instance;
}

}  // namespace gnash::history
