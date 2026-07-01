// histfile.cpp -- reading and writing the history file.
//
// Faithful to bash 5.3 lib/readline/histfile.c for the common cases: plain
// line-per-entry files and files carrying "#<seconds>" timestamp lines
// (written when write_timestamps is set).  A timestamp line applies to the
// entry that follows it.

#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "gnash/history.hpp"

namespace gnash::history {

namespace {

// Default file when the caller passes nullptr: $HOME/.history.
std::string default_history_file() {
  const char *home = std::getenv("HOME");
  if (home == nullptr || *home == '\0') return std::string();
  return std::string(home) + "/.history";
}

bool is_timestamp_line(const std::string &line, char comment_char) {
  return line.size() >= 2 && line[0] == comment_char &&
         std::isdigit(static_cast<unsigned char>(line[1]));
}

// Read the whole file into `out`.  Returns 0 or errno.
int slurp(const std::string &path, std::string &out) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return errno ? errno : ENOENT;
  std::ostringstream ss;
  ss << in.rdbuf();
  out = ss.str();
  return 0;
}

// Split into lines on '\n'.  A trailing newline does not yield an empty final
// element; a final line without a newline is kept.
std::vector<std::string> split_lines(const std::string &buf) {
  std::vector<std::string> lines;
  std::size_t start = 0;
  while (start <= buf.size()) {
    std::size_t nl = buf.find('\n', start);
    if (nl == std::string::npos) {
      if (start < buf.size()) lines.emplace_back(buf.substr(start));
      break;
    }
    lines.emplace_back(buf.substr(start, nl - start));
    start = nl + 1;
  }
  return lines;
}

}  // namespace

int History::read_file(const char *filename) {
  return read_file_range(filename, 0, -1);
}

int History::read_file_range(const char *filename, int from, int to) {
  std::string path = filename ? std::string(filename) : default_history_file();
  if (path.empty()) return ENOENT;

  std::string buf;
  if (int e = slurp(path, buf); e != 0) return e;

  std::vector<std::string> lines = split_lines(buf);
  const int nlines = static_cast<int>(lines.size());

  if (from < 0) from = 0;
  int last = (to < from) ? nlines : to;
  if (last > nlines) last = nlines;

  std::string pending_ts;
  bool have_ts = false;
  int count = 0;
  for (int i = from; i < last; i++) {
    const std::string &ln = lines[static_cast<std::size_t>(i)];
    if (is_timestamp_line(ln, comment_char)) {
      pending_ts = ln;
      have_ts = true;
      continue;
    }
    add(ln);
    if (have_ts) {
      add_time(pending_ts);
      have_ts = false;
    }
    count++;
  }

  lines_read_from_file = count;
  return 0;
}

// Shared writer for write_file (truncate) and append_file (append).
static int write_entries(History &h, const char *filename, int start,
                         bool append_mode, int &written) {
  written = 0;

  std::string path = filename ? std::string(filename) : default_history_file();
  if (path.empty()) return ENOENT;

  std::ios::openmode mode = std::ios::binary | std::ios::out;
  mode |= append_mode ? std::ios::app : std::ios::trunc;
  std::ofstream out(path, mode);
  if (!out) return errno ? errno : EACCES;

  Entry *const *v = h.list();
  for (int i = 0; v[i] != nullptr; i++) {
    if (i < start) continue;
    const Entry *e = v[i];
    if (h.write_timestamps && e->timestamp && e->timestamp[0])
      out << e->timestamp << '\n';
    out << (e->line ? e->line : "") << '\n';
    written++;
  }
  return out.good() ? 0 : (errno ? errno : EIO);
}

int History::write_file(const char *filename) {
  int written = 0;
  int r = write_entries(*this, filename, 0, /*append=*/false, written);
  lines_written_to_file = written;
  return r;
}

int History::append_file(int nelement, const char *filename) {
  int start = length() - nelement;
  if (start < 0) start = 0;
  int written = 0;
  int r = write_entries(*this, filename, start, /*append=*/true, written);
  lines_written_to_file = written;
  return r;
}

int History::truncate_file(const char *filename, int nlines) {
  std::string path = filename ? std::string(filename) : default_history_file();
  if (path.empty()) return ENOENT;

  std::string buf;
  if (int e = slurp(path, buf); e != 0) return e;

  std::vector<std::string> lines = split_lines(buf);

  // Indices of the actual entry (non-timestamp) lines.
  std::vector<int> entry_idx;
  for (int i = 0; i < static_cast<int>(lines.size()); i++) {
    if (!is_timestamp_line(lines[static_cast<std::size_t>(i)], comment_char))
      entry_idx.push_back(i);
  }

  if (nlines < 0) nlines = 0;
  const int total = static_cast<int>(entry_idx.size());
  if (nlines >= total) return 0;  // nothing to trim

  int keep_from_entry = total - nlines;
  int start_line = entry_idx[static_cast<std::size_t>(keep_from_entry)];
  // Keep an immediately-preceding timestamp line with its entry.
  if (start_line > 0 &&
      is_timestamp_line(lines[static_cast<std::size_t>(start_line - 1)], comment_char))
    start_line--;

  std::ofstream out(path, std::ios::binary | std::ios::out | std::ios::trunc);
  if (!out) return errno ? errno : EACCES;
  for (int i = start_line; i < static_cast<int>(lines.size()); i++)
    out << lines[static_cast<std::size_t>(i)] << '\n';

  return out.good() ? 0 : (errno ? errno : EIO);
}

}  // namespace gnash::history
