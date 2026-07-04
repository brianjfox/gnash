// Copyright (c) 2026 Brian J. Fox
// Licensed under GPLv2 with the GPLv2-AI Exception.

// prompt.cpp -- PS1/PS2 prompt-string expansion (bash backslash escapes).

#include "gnash/core/expand.hpp"
#include "readline/history.h"
#include "gnash/core/shell.hpp"

#include <cctype>
#include <cstdlib>
#include <ctime>
#include <vector>
#include <pwd.h>
#include <unistd.h>

namespace gnash::core {

namespace {
std::string home_relative(const std::string &cwd, const std::string &home) {
  if (!home.empty() && cwd.rfind(home, 0) == 0)
    return "~" + cwd.substr(home.size());
  return cwd;
}
}  // namespace

// Keep only the last N path components of DIR (0 = keep all).  A leading "~"
// or "/" is preserved.
static std::string last_components(const std::string &dir, int n) {
  if (n <= 0) return dir;
  std::vector<std::string> parts;
  std::string lead;
  size_t i = 0;
  if (!dir.empty() && (dir[0] == '/' || dir[0] == '~')) { lead = dir.substr(0, 1); i = 1; }
  while (i < dir.size()) {
    while (i < dir.size() && dir[i] == '/') i++;
    size_t j = i;
    while (j < dir.size() && dir[j] != '/') j++;
    if (j > i) parts.push_back(dir.substr(i, j - i));
    i = j;
  }
  if (static_cast<int>(parts.size()) <= n) return dir;
  std::string r;
  for (size_t k = parts.size() - static_cast<size_t>(n); k < parts.size(); k++) {
    if (!r.empty()) r += '/';
    r += parts[k];
  }
  return r;
}

// zsh prompt expansion: escapes are introduced by `%' rather than `\', and may
// carry a numeric argument (e.g. %1~ = last cwd component).
// zsh and csh share `%'-style prompt escapes.  `csh' selects the few points
// where they differ: csh's `%#' is `>'/`#' (zsh's is `%'/`#').
static std::string expand_prompt_pct(Shell &sh, const std::string &ps, bool csh) {
  std::string out;
  for (size_t i = 0; i < ps.size(); i++) {
    if (ps[i] != '%') { out += ps[i]; continue; }
    if (i + 1 >= ps.size()) { out += '%'; break; }
    int num = -1;
    while (i + 1 < ps.size() && std::isdigit(static_cast<unsigned char>(ps[i + 1]))) {
      num = (num < 0 ? 0 : num) * 10 + (ps[++i] - '0');
    }
    if (i + 1 >= ps.size()) { out += '%'; break; }
    char c = ps[++i];
    switch (c) {
      case 'n': {
        struct passwd *pw = getpwuid(getuid());
        out += pw ? pw->pw_name : (std::getenv("USER") ? std::getenv("USER") : "");
        break;
      }
      case 'm': case 'M': {
        char host[256] = {0};
        gethostname(host, sizeof host - 1);
        std::string h = host;
        if (c == 'm') { size_t d = h.find('.'); if (d != std::string::npos) h = h.substr(0, d); }
        out += h;
        break;
      }
      case '~': case 'd': case '/': case 'c': case 'C': case '.': {
        std::string dir = sh.get("PWD");
        if (dir.empty() || dir[0] != '/') { char cwd[4096]; dir = getcwd(cwd, sizeof cwd) ? cwd : ""; }
        if (c == '~' || c == 'c' || c == 'C' || c == '.') dir = home_relative(dir, sh.get("HOME"));
        // %c/%C/%. show the trailing component by default.
        int keep = num;
        if (keep < 0 && (c == 'c' || c == 'C' || c == '.')) keep = 1;
        out += last_components(dir, keep);
        break;
      }
      case '#': out += (getuid() == 0 ? '#' : (csh ? '>' : '%')); break;
      case '!': case 'h':  // history number (both shells)
        out += std::to_string(history_base + history_length);
        break;
      case '%': out += '%'; break;
      case 'T': case '*': case 't': {
        std::time_t now = std::time(nullptr);
        std::tm tmv; localtime_r(&now, &tmv);
        char buf[32];
        std::strftime(buf, sizeof buf, (c == '*') ? "%H:%M:%S" : "%H:%M", &tmv);
        out += buf;
        break;
      }
      case 'D': {
        std::time_t now = std::time(nullptr);
        std::tm tmv; localtime_r(&now, &tmv);
        char buf[32];
        std::strftime(buf, sizeof buf, "%y-%m-%d", &tmv);
        out += buf;
        break;
      }
      // Visual/format directives and parser state -- accepted, no output here.
      case 'B': case 'b': case 'U': case 'u': case 'S': case 's':
      case 'f': case 'k': case '_': case 'E': break;
      case 'F': case 'K': {  // %F{color}/%K{color}: skip the optional {...}
        if (i + 1 < ps.size() && ps[i + 1] == '{') { while (i < ps.size() && ps[i] != '}') i++; }
        break;
      }
      default: out += '%'; out += c; break;
    }
  }
  return out;
}

std::string expand_prompt(Shell &sh, const std::string &ps) {
  if (sh.is_zsh()) return expand_prompt_pct(sh, ps, false);
  if (sh.is_csh()) {
    // csh: a bare `!' in the prompt is the history number (`\!' is a literal
    // `!'); the rest are `%'-escapes.
    std::string tmp;
    for (size_t i = 0; i < ps.size(); i++) {
      if (ps[i] == '\\' && i + 1 < ps.size() && ps[i + 1] == '!') { tmp += '!'; i++; }
      else if (ps[i] == '!') tmp += std::to_string(history_base + history_length);
      else tmp += ps[i];
    }
    return expand_prompt_pct(sh, tmp, true);
  }
  // ash/POSIX: the prompt is subject to parameter/command/arithmetic expansion,
  // not backslash escapes.
  if (sh.is_ash()) return Expander(sh).expand_no_split(ps);
  if (sh.is_ksh()) {
    // ksh: like POSIX, but an unescaped `!' expands to the command's history
    // number (`!!' is a literal `!'); parameter expansion follows.
    std::string tmp;
    for (size_t i = 0; i < ps.size(); i++) {
      if (ps[i] != '!') { tmp += ps[i]; continue; }
      if (i + 1 < ps.size() && ps[i + 1] == '!') { tmp += '!'; i++; }
      else tmp += std::to_string(history_base + history_length);
    }
    return Expander(sh).expand_no_split(tmp);
  }
  std::string out;
  for (size_t i = 0; i < ps.size(); i++) {
    if (ps[i] != '\\') {
      out += ps[i];
      continue;
    }
    if (i + 1 >= ps.size()) { out += '\\'; break; }
    char c = ps[++i];
    switch (c) {
      case 'u': {
        struct passwd *pw = getpwuid(getuid());
        out += pw ? pw->pw_name : (std::getenv("USER") ? std::getenv("USER") : "");
        break;
      }
      case 'h': case 'H': {
        char host[256] = {0};
        gethostname(host, sizeof host - 1);
        std::string h = host;
        if (c == 'h') { size_t d = h.find('.'); if (d != std::string::npos) h = h.substr(0, d); }
        out += h;
        break;
      }
      case 'w': case 'W': {
        std::string dir = sh.get("PWD");  // logical path, as bash uses
        if (dir.empty() || dir[0] != '/') {
          char cwd[4096];
          dir = getcwd(cwd, sizeof cwd) ? cwd : "";
        }
        std::string home = sh.get("HOME");
        dir = home_relative(dir, home);
        if (c == 'W') { size_t s = dir.find_last_of('/'); if (s != std::string::npos && dir != "/") dir = dir.substr(s + 1); }
        out += dir;
        break;
      }
      case '$': out += (getuid() == 0 ? '#' : '$'); break;
      case '#': out += std::to_string(sh.command_number); break;      // command number
      case '!': out += std::to_string(history_base + history_length); break;  // history number
      case 'j': out += std::to_string(sh.jobs.size()); break;          // number of managed jobs
      case 'n': out += '\n'; break;
      case 'r': out += '\r'; break;
      case 'a': out += '\a'; break;
      case 'e': out += '\033'; break;
      case 's': {
        std::string a0 = sh.arg0;
        size_t s = a0.find_last_of('/');
        out += (s == std::string::npos) ? a0 : a0.substr(s + 1);
        break;
      }
      case 'v': case 'V': out += "0.1"; break;
      case 't': case 'T': case '@': case 'A': {
        std::time_t now = std::time(nullptr);
        std::tm tmv;
        localtime_r(&now, &tmv);
        char buf[32];
        const char *fmt = (c == 'A') ? "%H:%M" : (c == '@') ? "%I:%M %p" : (c == 'T') ? "%I:%M:%S" : "%H:%M:%S";
        std::strftime(buf, sizeof buf, fmt, &tmv);
        out += buf;
        break;
      }
      case 'd': {
        std::time_t now = std::time(nullptr);
        std::tm tmv;
        localtime_r(&now, &tmv);
        char buf[32];
        std::strftime(buf, sizeof buf, "%a %b %d", &tmv);
        out += buf;
        break;
      }
      case '[': case ']': break;  // non-printing markers
      case '\\': out += '\\'; break;
      default: out += '\\'; out += c; break;
    }
  }
  return out;
}

}  // namespace gnash::core
