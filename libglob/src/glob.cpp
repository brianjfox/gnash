// Copyright (c) 2026 Brian J. Fox
// Licensed under GPLv2 with the GPLv2-AI Exception.

// glob.cpp -- filename globbing built on the strmatch engine.
//
// A component-by-component recursive expander in the spirit of bash 5.3
// lib/glob/glob.c: split the pattern on `/`, match each magic component against
// the entries of the directory built so far, descend into matching directories,
// and support `**` (globstar).  Dotfile handling comes from FNM_PERIOD.  Results
// are sorted (C/ASCII order); duplicates from `**` decompositions are kept.

#include <algorithm>
#include <cstring>
#include <dirent.h>
#include <string>
#include <sys/stat.h>
#include <vector>

#include "gnash/glob.hpp"
#include "gnash/sh/xmalloc.hpp"
#include "glob.h"
#include "strmatch.h"

namespace {

bool is_dir(const std::string &path) {
  struct stat st;
  return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

// Like is_dir, but does NOT follow symlinks: a symlink to a directory is not a
// directory here.  Used by the globstar descent so `**' never recurses through
// a symlinked directory -- matching bash, and avoiding infinite recursion on a
// self-referential or cyclic symlink.
bool is_dir_nofollow(const std::string &path) {
  struct stat st;
  return lstat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool path_exists(const std::string &path) {
  struct stat st;
  return lstat(path.c_str(), &st) == 0;
}

bool has_magic(const std::string &s) {
  for (size_t i = 0; i < s.size(); i++) {
    char c = s[i];
    if (c == '*' || c == '?' || c == '[') return true;
    if ((c == '?' || c == '*' || c == '+' || c == '@' || c == '!') && i + 1 < s.size() &&
        s[i + 1] == '(')
      return true;
    if (c == '\\') i++;
  }
  return false;
}

// Remove backslash quoting from a pattern component that will be used as a
// literal pathname: `t\mp' names the directory `tmp', `tmp\' (a backslash
// that quoted the following `/' before the pattern was split) names `tmp'.
// bash dequotes every non-pattern component the same way (glob.c:glob_filename
// via dequote_pathname).
std::string dequote(const std::string &s) {
  std::string out;
  for (size_t i = 0; i < s.size(); i++) {
    if (s[i] == '\\') {
      if (i + 1 < s.size()) out += s[++i];
      continue;  // a trailing backslash quotes nothing and disappears
    }
    out += s[i];
  }
  return out;
}

bool sm_match(const std::string &pat, const std::string &name, int smflags) {
  std::string p = pat, n = name;
  return strmatch(p.data(), n.data(), smflags) == 0;
}

// Names in `dirpath` ("" means ".") matching `pat`.
std::vector<std::string> match_dir(const std::string &dirpath, const std::string &pat,
                                   int smflags, bool skipdots) {
  std::vector<std::string> out;
  DIR *d = opendir(dirpath.empty() ? "." : dirpath.c_str());
  if (!d) return out;
  struct dirent *e;
  while ((e = readdir(d)) != nullptr) {
    // bash's GLOBSKIPDOTS (on by default): `.' and `..' are excluded; with it
    // off they are matched like any other name.
    if (skipdots && e->d_name[0] == '.' &&
        (e->d_name[1] == '\0' || (e->d_name[1] == '.' && e->d_name[2] == '\0')))
      continue;
    if (sm_match(pat, e->d_name, smflags)) out.emplace_back(e->d_name);
  }
  closedir(d);
  return out;
}

// All directories at or below `base` (each with a trailing `/`), for globstar.
void collect_dirs(const std::string &base, std::vector<std::string> &out, bool matchdot) {
  out.push_back(base);
  DIR *d = opendir(base.empty() ? "." : base.c_str());
  if (!d) return;
  std::vector<std::string> subs;
  struct dirent *e;
  while ((e = readdir(d)) != nullptr) {
    std::string nm = e->d_name;
    if (nm == "." || nm == "..") continue;
    if (nm[0] == '.' && !matchdot) continue;
    std::string full = base + nm;
    if (is_dir_nofollow(full)) subs.push_back(full + "/");  // don't descend symlinks
  }
  closedir(d);
  std::sort(subs.begin(), subs.end());
  for (auto &s : subs) collect_dirs(s, out, matchdot);
}

// literal_prefix is true when every path component consumed to reach `dir' was
// a plain literal (no `**' or other glob).  It controls the trailing-slash form
// of the terminal `**' zero-segment match (see below).
void glob_recurse(const std::string &dir, const std::string &pat, int smflags, bool matchdot,
                  int gxflags, std::vector<std::string> &results, bool literal_prefix = true) {
  size_t slash = pat.find('/');
  std::string head = (slash == std::string::npos) ? pat : pat.substr(0, slash);
  std::string rest = (slash == std::string::npos) ? "" : pat.substr(slash + 1);
  bool last = (slash == std::string::npos);

  // globstar: ** matches zero or more directory levels.
  if ((gxflags & GX_GLOBSTAR) && head == "**") {
    // Collapse a run of `**/'...`**' components into a single `**' (bash does
    // this so `**/**' is equivalent to `**').  Absorb each following `**/' (or
    // trailing `**') into this one.
    bool collapsed = false;
    while (!last && (rest == "**" || rest.compare(0, 3, "**/") == 0)) {
      collapsed = true;
      if (rest == "**") {
        rest.clear();
        last = true;
      } else {
        rest.erase(0, 3);
        while (!rest.empty() && rest[0] == '/') rest.erase(0, 1);
        if (rest.empty()) last = true;
      }
    }
    std::vector<std::string> dirs;
    collect_dirs(dir, dirs, matchdot);
    if (last) {
      // `**' matches zero path segments too, i.e. the base directory itself.
      // bash renders this base with a trailing slash only when the whole prefix
      // was a single literal path followed directly by a terminal `**'
      // (`a/**' -> `a/'); any `**'/glob ancestor or a collapsed `**/**' yields
      // the base without the slash (`a/**/**', `**/a/**' -> `a').  A bare `**'
      // (empty dir) contributes no base.
      if (!dir.empty()) {
        if (literal_prefix && !collapsed) {
          results.push_back(dir);
        } else {
          std::string base = dir;
          if (base.size() > 1 && base.back() == '/') base.pop_back();
          results.push_back(base);
        }
      }
      for (const std::string &D : dirs) {
        DIR *dd = opendir(D.empty() ? "." : D.c_str());
        if (!dd) continue;
        struct dirent *e;
        while ((e = readdir(dd)) != nullptr) {
          std::string nm = e->d_name;
          if (nm == "." || nm == "..") continue;
          if (nm[0] == '.' && !matchdot) continue;
          results.push_back(D + nm);
        }
        closedir(dd);
      }
    } else {
      for (const std::string &D : dirs)
        glob_recurse(D, rest, smflags, matchdot, gxflags, results, /*literal_prefix=*/false);
      // A trailing `**/' also matches SYMLINKS to directories (`ln -s a c'
      // lists `c/'), even though the globstar descent never follows them.
      if (rest.empty()) {
        for (const std::string &D : dirs) {
          DIR *dd = opendir(D.empty() ? "." : D.c_str());
          if (!dd) continue;
          struct dirent *e;
          while ((e = readdir(dd)) != nullptr) {
            std::string nm = e->d_name;
            if (nm == "." || nm == "..") continue;
            if (nm[0] == '.' && !matchdot) continue;
            std::string full = D + nm;
            if (!is_dir_nofollow(full) && is_dir(full)) results.push_back(full + "/");
          }
          closedir(dd);
        }
      }
    }
    return;
  }

  if (!has_magic(head)) {
    std::string next = dir + dequote(head);
    if (last) {
      if (path_exists(next)) results.push_back(next);
    } else if (is_dir(next)) {
      glob_recurse(next + "/", rest, smflags, matchdot, gxflags, results, literal_prefix);
    }
    return;
  }

  std::vector<std::string> names =
      match_dir(dir, head, smflags, (gxflags & GX_NODOTSKIP) == 0);
  std::sort(names.begin(), names.end());
  for (const std::string &nm : names) {
    std::string full = dir + nm;
    if (last) {
      if ((gxflags & GX_MATCHDIRS) && !is_dir(full)) continue;
      results.push_back((gxflags & GX_MARKDIRS) && is_dir(full) ? full + "/" : full);
    } else if (is_dir(full)) {
      glob_recurse(full + "/", rest, smflags, matchdot, gxflags, results, /*literal_prefix=*/false);
    }
  }
}

}  // namespace

// ---- C++ API --------------------------------------------------------------
namespace gnash::glob {

bool fnmatch(std::string_view pat, std::string_view str, int flags) {
  std::string p(pat), s(str);
  return strmatch(p.data(), s.data(), flags) == 0;
}

std::vector<std::string> glob(std::string_view pattern, int flags) {
  int sm = FNM_PATHNAME | FNM_EXTMATCH;
  bool matchdot = (flags & GX_MATCHDOT) != 0;
  // Mirror bash: a leading `.' is matched explicitly (FNM_PERIOD) unless dotglob
  // is on, in which case only `.'/`..' still require an explicit dot (FNM_DOTDOT).
  sm |= matchdot ? FNM_DOTDOT : FNM_PERIOD;
  if (flags & GX_NOCASE) sm |= FNM_CASEFOLD;

  std::string pat(pattern);
  std::string dir;
  if (!pat.empty() && pat[0] == '/') {
    dir = "/";
    pat.erase(0, 1);
  }

  std::vector<std::string> results;
  glob_recurse(dir, pat, sm, matchdot, flags, results);

  std::sort(results.begin(), results.end());
  // NO de-duplication: bash keeps one result per `**' decomposition, so
  // `**/a/**' over a/a/a yields <a/a/a> three times (once per way of
  // splitting the path between the two `**'s), exactly as ksh93 does.
  return results;
}

}  // namespace gnash::glob

// ---- C shims --------------------------------------------------------------
extern "C" int glob_pattern_p(const char *pattern) {
  return has_magic(pattern ? pattern : "") ? 1 : 0;
}

extern "C" char **glob_filename(char *pathname, int flags) {
  std::vector<std::string> v = gnash::glob::glob(pathname ? pathname : "", flags);
  if (v.empty()) return nullptr;
  char **ret =
      static_cast<char **>(gnash::sh::xmalloc((v.size() + 1) * sizeof(char *)));
  for (size_t i = 0; i < v.size(); i++) ret[i] = gnash::sh::savestring(v[i].c_str());
  ret[v.size()] = nullptr;
  return ret;
}
