// Copyright (c) 2026 Brian J. Fox
// Licensed under GPLv2 with the GPLv2-AI Exception.

// expand.cpp -- word expansion (see expand.hpp).
//
// A pragmatic but faithful implementation of the bash expansion pipeline.  Not
// yet covered: arrays (${a[@]}), ${!prefix*}, some locale/case operators; these
// are follow-ons.

#include "gnash/core/expand.hpp"

#include <algorithm>
#include <cctype>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <pwd.h>
#include <unistd.h>
#include <sys/wait.h>

#include "gnash/glob.hpp"
#include "glob.h"
#include "strmatch.h"

namespace gnash::core {

namespace {

constexpr char FIELD_SEP = '\x01';  // internal "$@" field boundary marker
// Quoted-null marker: emitted when a quote region opens, so an expansion that
// yields empty text still leaves a field ("" / "$empty" -> one empty field).
// A double-quoted "$@"/"${a[@]}" that expands to nothing absorbs the marker,
// so it yields *no* field (matching bash).  Stripped before the result is used.
constexpr char QNULL = '\x02';
// Mask letter for the marker bytes themselves: only a FIELD_SEP/QNULL whose
// mask is MMARK is an internal marker; the same byte under any other mask is
// literal data (e.g. $'\001').
constexpr char MMARK = 'M';

bool pat_match(const std::string &pattern, const std::string &text) {
  std::string p = pattern, t = text;
  return strmatch(p.data(), t.data(), FNM_EXTMATCH) == 0;
}

// Scan a balanced span from `open`/`close` starting with text[i] at the opener;
// returns the index of the matching closer (or npos).  Honors quotes.
size_t scan_balanced(const std::string &t, size_t i, char open, char close) {
  int depth = 0;
  for (; i < t.size(); i++) {
    char c = t[i];
    if (c == '\\') { i++; continue; }
    if (c == '$' && i + 1 < t.size() && t[i + 1] == '\'') {
      // $'...': ANSI-C quoting, where a backslash escapes the next character
      // (so \' is not a terminator).
      i += 2;
      while (i < t.size() && t[i] != '\'') { if (t[i] == '\\' && i + 1 < t.size()) i++; i++; }
      continue;
    }
    if (c == '\'') { while (++i < t.size() && t[i] != '\'') {} continue; }
    if (c == '"') {
      while (++i < t.size() && t[i] != '"')
        if (t[i] == '\\') i++;
      continue;
    }
    if (c == open) depth++;
    else if (c == close) { if (--depth == 0) return i; }
  }
  return std::string::npos;
}

// Encode a Unicode code point as UTF-8 (for \u / \U), matching bash's u32cconv.
void append_utf8(std::string &out, unsigned long v) {
  if (v <= 0x7f) {
    out += static_cast<char>(v);
  } else if (v <= 0x7ff) {
    out += static_cast<char>(0xc0 | (v >> 6));
    out += static_cast<char>(0x80 | (v & 0x3f));
  } else if (v <= 0xffff) {
    out += static_cast<char>(0xe0 | (v >> 12));
    out += static_cast<char>(0x80 | ((v >> 6) & 0x3f));
    out += static_cast<char>(0x80 | (v & 0x3f));
  } else {
    out += static_cast<char>(0xf0 | (v >> 18));
    out += static_cast<char>(0x80 | ((v >> 12) & 0x3f));
    out += static_cast<char>(0x80 | ((v >> 6) & 0x3f));
    out += static_cast<char>(0x80 | (v & 0x3f));
  }
}

// ANSI-C dequoting for $'...' -- matches bash's ansicstr(..., flags=2).  Beyond
// the single-letter escapes it decodes octal (\nnn), hex (\xHH and \x{...}),
// Unicode (\uHHHH, \UHHHHHHHH), and control (\cX) escapes into real bytes.  An
// unrecognized escape keeps its backslash, as bash does.
std::string ansi_c(const std::string &s) {
  auto hexval = [](char c) {
    return c <= '9' ? c - '0' : (std::tolower((unsigned char)c) - 'a' + 10);
  };
  std::string out;
  for (size_t i = 0; i < s.size(); i++) {
    if (s[i] != '\\' || i + 1 >= s.size()) { out += s[i]; continue; }
    char c = s[++i];
    switch (c) {
      case 'n': out += '\n'; break;
      case 't': out += '\t'; break;
      case 'r': out += '\r'; break;
      case 'a': out += '\a'; break;
      case 'b': out += '\b'; break;
      case 'f': out += '\f'; break;
      case 'v': out += '\v'; break;
      case '\\': out += '\\'; break;
      case '\'': out += '\''; break;
      case '"': out += '"'; break;
      case '?': out += '?'; break;
      case 'e': case 'E': out += '\033'; break;
      case '0': case '1': case '2': case '3':
      case '4': case '5': case '6': case '7': {
        // Octal: this digit plus up to two more (three total).
        int v = c - '0', k = 0;
        while (k < 2 && i + 1 < s.size() && s[i + 1] >= '0' && s[i + 1] <= '7') {
          v = v * 8 + (s[++i] - '0');
          k++;
        }
        out += static_cast<char>(v & 0xff);
        break;
      }
      case 'x': {
        // Hex: \xHH (1-2 digits) or brace form \x{HH...}.  A bare \x with no
        // hex digit is passed through unchanged; a brace form with no digits
        // terminates the whole string (bash's ansicstr bails out).
        bool brace = i + 1 < s.size() && s[i + 1] == '{';
        if (brace) i++;
        int v = 0, k = 0;
        int limit = brace ? INT_MAX : 2;
        while (k < limit && i + 1 < s.size() &&
               std::isxdigit((unsigned char)s[i + 1])) {
          v = v * 16 + hexval(s[++i]);
          k++;
        }
        if (brace && i + 1 < s.size() && s[i + 1] == '}') i++;
        if (brace && k == 0) return out;  // \x{ with no digits: drop the rest
        if (k == 0) { out += '\\'; out += 'x'; }
        else out += static_cast<char>(v & 0xff);
        break;
      }
      case 'u': case 'U': {
        // Unicode: \u up to 4 hex digits, \U up to 8; encoded as UTF-8.  With
        // no hex digit following, the backslash is kept.
        int limit = c == 'u' ? 4 : 8;
        unsigned long v = 0;
        int k = 0;
        while (k < limit && i + 1 < s.size() &&
               std::isxdigit((unsigned char)s[i + 1])) {
          v = v * 16 + static_cast<unsigned long>(hexval(s[++i]));
          k++;
        }
        if (k == 0) { out += '\\'; out += c; }
        else append_utf8(out, v);
        break;
      }
      case 'c': {
        // Control char: \cX -> TOCTRL(X).  At end of string, literal \c.
        if (i + 1 >= s.size()) { out += '\\'; out += 'c'; break; }
        char n = s[++i];
        if (n == '\\' && i + 1 < s.size() && s[i + 1] == '\\') i++;  // $'\c\\'
        out += static_cast<char>(n == '?' ? 0x7f
                                          : (std::toupper((unsigned char)n) & 0x1f));
        break;
      }
      default: out += '\\'; out += c; break;
    }
  }
  return out;
}

}  // namespace

// Expand a leading ~ / ~user / ~+ / ~- in WORD (the unquoted case).
static std::string expand_leading_tilde(Shell &sh, const std::string &w) {
  if (w.empty() || w[0] != '~') return w;
  size_t slash = w.find('/');
  std::string prefix = w.substr(1, (slash == std::string::npos ? w.size() : slash) - 1);
  std::string home;
  if (prefix.empty()) {
    home = sh.get("HOME");
    if (home.empty()) { const char *h = getenv("HOME"); if (h) home = h; }
  } else if (prefix == "+") {
    home = sh.get("PWD");
  } else if (prefix == "-") {
    home = sh.get("OLDPWD");
  } else if (prefix[0] == '+' || prefix[0] == '-' ||
             std::isdigit(static_cast<unsigned char>(prefix[0]))) {
    // ~N / ~+N / ~-N: the Nth directory-stack entry (bash's DIRSTACK).
    const char *dp = prefix.c_str();
    bool from_end = (*dp == '-');
    if (*dp == '+' || *dp == '-') dp++;
    bool alldig = *dp != '\0';
    for (const char *q = dp; *q; q++)
      if (!std::isdigit(static_cast<unsigned char>(*q))) alldig = false;
    if (alldig) {
      std::vector<std::string> ds = sh.dirstack();
      long k = std::atol(dp);
      long idx = from_end ? static_cast<long>(ds.size()) - 1 - k : k;
      if (idx >= 0 && static_cast<size_t>(idx) < ds.size()) home = ds[static_cast<size_t>(idx)];
      else return w;  // out of range: leave literal
    } else {
      struct passwd *pw = getpwnam(prefix.c_str());
      if (pw) home = pw->pw_dir;
    }
  } else {
    struct passwd *pw = getpwnam(prefix.c_str());
    if (pw) home = pw->pw_dir;
  }
  if (home.empty()) return w;
  // The substituted home directory is not re-scanned for expansions: escape
  // the shell specials so the later process() pass keeps them literal (e.g.
  // HOME='/usr/$x' must not expand `$x').
  std::string esc;
  for (char c : home) {
    if (c == '$' || c == '`' || c == '\\' || c == '"' || c == '\'' ||
        c == '*' || c == '?' || c == '[' || c == '~' || c == ' ' || c == '\t')
      esc += '\\';
    esc += c;
  }
  return esc + (slash == std::string::npos ? std::string() : w.substr(slash));
}

// Tilde expansion for an assignment value: leading ~ and ~ after each unquoted colon.
static std::string tilde_assign(Shell &sh, const std::string &text) {
  std::string out, cur;
  bool sq = false, dq = false;
  for (char c : text) {
    if (c == '\'' && !dq) { sq = !sq; cur += c; continue; }
    if (c == '"' && !sq) { dq = !dq; cur += c; continue; }
    if (c == ':' && !sq && !dq) { out += expand_leading_tilde(sh, cur); out += ':'; cur.clear(); continue; }
    cur += c;
  }
  out += expand_leading_tilde(sh, cur);
  return out;
}

std::string Expander::param_value(const std::string &name, bool &set, bool defaulting_op) {
  set = true;
  if (name == "?") return std::to_string(sh_.last_status);
  if (name == "$") return sh_.get("$");
  if (name == "!") return std::to_string(sh_.last_bg_pid);
  if (name == "#") return std::to_string(sh_.positional.size());
  if (name == "0") return sh_.arg0;
  if (name == "-") {
    // The current option flags, in bash's order: the single-letter set options
    // (alphabetical), then the always-on defaults B/H/m, then the invocation
    // letter (`c' for -c, `s' when reading from stdin).  hashall (h) and
    // braceexpand (B) are on by default, as in bash.
    std::string f;
    if (sh_.opt_errexit) f += 'e';
    if (sh_.opt_noglob) f += 'f';
    f += 'h';                       // hashall: on by default
    if (sh_.interactive) f += 'i';  // rc files test `case $- in *i*)'
    if (sh_.job_control) f += 'm';  // monitor: interactive job control
    if (sh_.opt_noexec) f += 'n';
    if (sh_.opt_nounset) f += 'u';
    if (sh_.opt_verbose) f += 'v';
    if (sh_.opt_xtrace) f += 'x';
    f += 'B';                       // braceexpand: on by default
    if (sh_.opt_histexpand) f += 'H';  // histexpand (set -H; interactive default)
    if (sh_.invocation_char) f += sh_.invocation_char;
    return f;
  }
  if (name == "*" || name == "@") {
    // Unset exactly when there are no positional parameters (so ${*-x}
    // defaults only for $# == 0); the value joins with the first IFS char.
    set = !sh_.positional.empty();
    std::string ifs = sh_.ifs();
    char sep = ifs.empty() ? ' ' : ifs[0];
    std::string r;
    for (size_t k = 0; k < sh_.positional.size(); k++) {
      if (k) r += sep;
      r += sh_.positional[k];
    }
    return r;
  }
  if (!name.empty() && std::isdigit(static_cast<unsigned char>(name[0]))) {
    size_t idx = static_cast<size_t>(std::atoi(name.c_str()));
    if (idx >= 1 && idx <= sh_.positional.size()) return sh_.positional[idx - 1];
    set = false;
    return std::string();
  }
  std::string v;
  if (sh_.get_if_set(name, v)) return v;
  if (sh_.dynamic_var(name, v)) return v;  // RANDOM/SECONDS/LINENO/BASHPID/EPOCH*
  set = false;
  if (sh_.opt_nounset && !defaulting_op) {
    std::fprintf(stderr, "%s%s: unbound variable\n", sh_.err_prefix().c_str(), name.c_str());
    sh_.exiting = true;
    sh_.exit_status = 127;  // bash exits a non-interactive shell with 127 here
  }
  return std::string();
}

// Expand a ${...} body (without the braces).  Returns the value; sets `split`
// if the result is subject to word splitting (always, for consistency here).
static std::string expand_brace_body(Expander &, Shell &, const std::string &, bool dq);
static std::string apply_param_op(Expander &, Shell &, const std::string &name,
                                  std::string val, bool set, const std::string &rest, bool dq);

// Detect NAME[@] / NAME[*] with an optional leading `#' (count) or `!' (keys).
static bool array_ref(const std::string &body, char &lead, std::string &name, char &sel) {
  size_t p = 0;
  lead = 0;
  if (!body.empty() && (body[0] == '#' || body[0] == '!')) { lead = body[0]; p = 1; }
  size_t s = p;
  while (p < body.size() && (std::isalnum(static_cast<unsigned char>(body[p])) || body[p] == '_')) p++;
  name = body.substr(s, p - s);
  if (name.empty()) return false;
  if (p + 3 == body.size() && body[p] == '[' && (body[p + 1] == '@' || body[p + 1] == '*') &&
      body[p + 2] == ']') {
    sel = body[p + 1];
    return true;
  }
  return false;
}

// Detect NAME[@]OP / NAME[*]OP where OP is an operator applied element-wise
// (case-mod ^ , ; pattern removal # % ; substitution / ; transform @).  Slicing
// (:offset) and default (:-) forms act on the array as a whole and are excluded.
static bool array_op_ref(const std::string &body, std::string &name, char &sel,
                         std::string &rest) {
  if (body.empty() || !(std::isalpha(static_cast<unsigned char>(body[0])) || body[0] == '_'))
    return false;
  size_t p = 0;
  while (p < body.size() && (std::isalnum(static_cast<unsigned char>(body[p])) || body[p] == '_')) p++;
  name = body.substr(0, p);
  if (p + 3 > body.size() || body[p] != '[' ||
      (body[p + 1] != '@' && body[p + 1] != '*') || body[p + 2] != ']')
    return false;
  sel = body[p + 1];
  rest = body.substr(p + 3);
  if (rest.empty()) return false;
  char c = rest[0];
  return c == '^' || c == ',' || c == '~' || c == '#' || c == '%' || c == '/' ||
         c == '@';
}

// Detect array/positional slicing: NAME[@]:off[:len] / NAME[*]:off[:len] and
// the positional @:off[:len] / *:off[:len].  offx/lenx are the raw arithmetic
// offset and length; haslen indicates whether a length was given.
static bool slice_ref(const std::string &body, std::string &name, char &sel,
                      std::string &offx, std::string &lenx, bool &haslen) {
  size_t p = 0;
  if (!body.empty() && (body[0] == '@' || body[0] == '*') && body.size() > 1 && body[1] == ':') {
    name.clear();
    sel = body[0];
    p = 1;
  } else {
    if (body.empty() || !(std::isalpha(static_cast<unsigned char>(body[0])) || body[0] == '_'))
      return false;
    while (p < body.size() && (std::isalnum(static_cast<unsigned char>(body[p])) || body[p] == '_')) p++;
    if (p + 4 > body.size() || body[p] != '[' ||
        (body[p + 1] != '@' && body[p + 1] != '*') || body[p + 2] != ']' || body[p + 3] != ':')
      return false;
    name = body.substr(0, p);
    sel = body[p + 1];
    p += 3;
  }
  if (p >= body.size() || body[p] != ':') return false;
  std::string tail = body.substr(p + 1);
  size_t colon = tail.find(':');
  if (colon == std::string::npos) { offx = tail; haslen = false; }
  else { offx = tail.substr(0, colon); lenx = tail.substr(colon + 1); haslen = true; }
  return true;
}

bool Expander::emit_zsh_flags(const std::string &body, bool dq, std::string &out,
                              std::string &mask) {
  if (body.empty() || body[0] != '(') return false;
  size_t rp = body.find(')');
  if (rp == std::string::npos) return false;
  std::string flags = body.substr(1, rp - 1);
  std::string rest = body.substr(rp + 1);
  if (rest.empty()) return false;

  bool f_join = false, f_split = false, f_sort = false, f_rev = false, f_num = false;
  bool f_ci = false, f_uniq = false, f_keys = false, f_vals = false;
  int f_case = 0;  // 1=L 2=U 3=C
  std::string jsep, ssep;
  for (size_t p = 0; p < flags.size(); p++) {
    char c = flags[p];
    if ((c == 'j' || c == 's') && p + 1 < flags.size()) {
      char d = flags[p + 1];
      size_t q = p + 2;
      std::string sep;
      while (q < flags.size() && flags[q] != d) sep += flags[q++];
      if (c == 'j') { jsep = sep; f_join = true; } else { ssep = sep; f_split = true; }
      p = q;  // at the closing delimiter; loop ++ steps past it
      continue;
    }
    switch (c) {
      case 'F': jsep = "\n"; f_join = true; break;
      case 'f': ssep = "\n"; f_split = true; break;
      case 'o': f_sort = true; break;
      case 'O': f_sort = true; f_rev = true; break;
      case 'n': f_num = true; break;
      case 'i': f_ci = true; break;
      case 'u': f_uniq = true; break;
      case 'L': f_case = 1; break;
      case 'U': f_case = 2; break;
      case 'C': f_case = 3; break;
      case 'k': f_keys = true; break;
      case 'v': f_vals = true; break;
      default: break;  // unrecognized flags are ignored
    }
  }

  // Resolve the base parameter (name, name[sub], or @/*) to a list of values.
  size_t q = 0;
  std::string nm;
  if (rest[0] == '@' || rest[0] == '*') { nm = rest.substr(0, 1); q = 1; }
  else { while (q < rest.size() && (std::isalnum((unsigned char)rest[q]) || rest[q] == '_')) q++;
         nm = rest.substr(0, q); }
  std::string sub;
  bool have_sub = false;
  if (q < rest.size() && rest[q] == '[') {
    size_t s = q + 1, p2 = q + 1;
    int d = 1;
    while (p2 < rest.size() && d) { if (rest[p2] == '[') d++; else if (rest[p2] == ']') d--; if (d) p2++; }
    sub = rest.substr(s, p2 - s);
    have_sub = true;
  }

  std::vector<std::string> items;
  if (nm.empty()) {
    // The operand is a nested expansion, e.g. ${(s:+:)$(cmd)} or ${(f)${x}};
    // expand it to a scalar and let the split/transform flags act on it.
    items.push_back(expand_no_split(rest));
  } else if (nm == "@" || nm == "*") {
    items = sh_.positional;
  } else if (f_keys && f_vals) {
    std::vector<std::string> k = sh_.array_keys(nm), v = sh_.array_values(nm);
    for (size_t x = 0; x < k.size() && x < v.size(); x++) { items.push_back(k[x]); items.push_back(v[x]); }
  } else if (f_keys) {
    items = sh_.array_keys(nm);
  } else if (have_sub && sub != "@" && sub != "*") {
    items.push_back(sh_.array_get(nm, sh_.zsh_subscript(nm, expand_no_split(sub))));
  } else if (sh_.is_array(nm)) {
    items = sh_.array_values(nm);
  } else {
    items.push_back(sh_.get(nm));
  }

  // split (s/f): split each element on the separator.
  if (f_split) {
    std::vector<std::string> out2;
    for (const std::string &it : items) {
      if (ssep.empty()) { out2.push_back(it); continue; }
      size_t pos = 0, nx;
      while ((nx = it.find(ssep, pos)) != std::string::npos) { out2.push_back(it.substr(pos, nx - pos)); pos = nx + ssep.size(); }
      out2.push_back(it.substr(pos));
    }
    items.swap(out2);
  }
  // case (L/U/C).
  if (f_case) {
    for (std::string &it : items) {
      if (f_case == 1) for (char &c : it) c = std::tolower((unsigned char)c);
      else if (f_case == 2) for (char &c : it) c = std::toupper((unsigned char)c);
      else {  // C: capitalize the first letter of each word
        bool start = true;
        for (char &c : it) {
          if (std::isalnum((unsigned char)c)) { c = start ? std::toupper((unsigned char)c) : std::tolower((unsigned char)c); start = false; }
          else start = true;
        }
      }
    }
  }
  // unique (u): drop later duplicates, keep first-seen order.
  if (f_uniq) {
    std::vector<std::string> out2;
    for (const std::string &it : items)
      if (std::find(out2.begin(), out2.end(), it) == out2.end()) out2.push_back(it);
    items.swap(out2);
  }
  // sort (o/O), optionally numeric (n) / case-insensitive (i).
  if (f_sort) {
    auto lower = [](std::string s) { for (char &c : s) c = std::tolower((unsigned char)c); return s; };
    std::stable_sort(items.begin(), items.end(), [&](const std::string &a, const std::string &b) {
      if (f_num) { long long x = std::atoll(a.c_str()), y = std::atoll(b.c_str()); if (x != y) return x < y; }
      if (f_ci) return lower(a) < lower(b);
      return a < b;
    });
    if (f_rev) std::reverse(items.begin(), items.end());
  }
  // join (j/F): collapse to a single scalar.
  if (f_join) {
    std::string joined;
    for (size_t x = 0; x < items.size(); x++) { if (x) joined += jsep; joined += items[x]; }
    items.assign(1, joined);
    char qm = dq ? '1' : '0';
    for (char c : joined) { out += c; mask += qm; }
    return true;
  }
  // Emit the list: one word per element (unquoted) or IFS-joined (double-quoted).
  if (dq) {
    std::string is = sh_.ifs();
    std::string joiner = is.empty() ? std::string() : std::string(1, is[0]);
    for (size_t x = 0; x < items.size(); x++) {
      if (x) for (char c : joiner) { out += c; mask += '1'; }
      for (char c : items[x]) { out += c; mask += '1'; }
    }
  } else {
    for (size_t x = 0; x < items.size(); x++) {
      if (x) { out += FIELD_SEP; mask += MMARK; }
      for (char c : items[x]) { out += c; mask += '0'; }
    }
  }
  return true;
}

void Expander::emit_zsh_subscript(const std::string &name, const std::string &sub, bool dq,
                                  std::string &out, std::string &mask) {
  // A top-level comma makes this a zsh range `lo,hi' (both 1-based, inclusive,
  // negatives counting from the end).  Otherwise it is a single index.
  size_t comma = std::string::npos;
  for (size_t k = 0; k < sub.size(); k++) {
    if (sub[k] == '[') { int d = 1; while (++k < sub.size() && d) d += (sub[k]=='[') - (sub[k]==']'); }
    else if (sub[k] == ',') { comma = k; break; }
  }
  char qm = dq ? '1' : '0';
  // Scalar subscripting is character (single) / substring (range) selection,
  // 1-based, negatives counting from the end -- e.g. s=hello, $s[1]=h, $s[2,4]=ell.
  if (!sh_.is_array(name)) {
    std::string val = sh_.get(name);
    long long n = static_cast<long long>(val.size());
    bool ok = true;
    long long lo, hi;
    if (comma != std::string::npos) {
      lo = eval_arith(sh_, expand_no_split(sub.substr(0, comma)), &ok);
      hi = eval_arith(sh_, expand_no_split(sub.substr(comma + 1)), &ok);
    } else {
      lo = hi = eval_arith(sh_, expand_no_split(sub), &ok);
    }
    if (lo < 0) lo += n + 1;
    if (hi < 0) hi += n + 1;
    if (lo < 1) lo = 1;
    for (long long k = lo; k <= hi && k <= n; k++)
      if (k >= 1) { out += val[static_cast<size_t>(k - 1)]; mask += qm; }
    return;
  }
  if (comma != std::string::npos) {
    std::vector<std::string> all = sh_.array_values(name);
    long long n = static_cast<long long>(all.size());
    bool ok = true;
    long long lo = eval_arith(sh_, expand_no_split(sub.substr(0, comma)), &ok);
    long long hi = eval_arith(sh_, expand_no_split(sub.substr(comma + 1)), &ok);
    if (lo < 0) lo += n + 1;  // -1 == last element (position n)
    if (hi < 0) hi += n + 1;
    std::vector<std::string> items;
    for (long long k = lo; k <= hi; k++)
      if (k >= 1 && k <= n) items.push_back(all[static_cast<size_t>(k - 1)]);
    if (dq) {
      std::string is = sh_.ifs();
      std::string joiner = is.empty() ? std::string() : std::string(1, is[0]);
      for (size_t k = 0; k < items.size(); k++) {
        if (k) for (char c : joiner) { out += c; mask += '1'; }
        for (char c : items[k]) { out += c; mask += '1'; }
      }
    } else {
      for (size_t k = 0; k < items.size(); k++) {
        if (k) { out += FIELD_SEP; mask += MMARK; }
        for (char c : items[k]) { out += c; mask += '0'; }
      }
    }
    return;
  }
  std::string val = sh_.array_get(name, sh_.zsh_subscript(name, expand_no_split(sub)));
  for (char c : val) { out += c; mask += qm; }
}

void Expander::expand_dollar(const std::string &t, size_t &i, bool dq, std::string &out,
                             std::string &mask, bool heredoc) {
  char qm = dq ? '1' : '0';
  // i is at '$'
  char n1 = i + 1 < t.size() ? t[i + 1] : '\0';
  // A double-quoted "$@"/"${a[@]}" manages its own fields, so it absorbs the
  // quoted-null the opening quote emitted -- letting an empty list drop the word.
  auto absorb_qnull = [&]() {
    if (!out.empty() && out.back() == QNULL && mask.back() == MMARK) {
      out.pop_back();
      mask.pop_back();
    }
  };

  // $((expr))
  if (n1 == '(' && i + 2 < t.size() && t[i + 2] == '(') {
    size_t p = i + 3;
    int depth = 2;
    size_t end = std::string::npos;
    for (; p < t.size(); p++) {
      if (t[p] == '(') depth++;
      else if (t[p] == ')') { if (--depth == 0) { end = p; break; } }
    }
    if (end != std::string::npos) {
      std::string expr = t.substr(i + 3, (end - 1) - (i + 3));
      bool ok = true;
      long long v = eval_arith_msg(sh_, expand_no_split(expr), "", &ok);
      if (!ok) { sh_.arith_error = true; i = end + 1; return; }
      std::string s = std::to_string(v);
      for (char c : s) { out += c; mask += qm; }
      i = end + 1;
      return;
    }
  }
  // $[expr] -- deprecated arithmetic expansion
  if (n1 == '[') {
    size_t end = scan_balanced(t, i + 1, '[', ']');
    if (end != std::string::npos) {
      std::string expr = t.substr(i + 2, end - (i + 2));
      bool ok = true;
      long long v = eval_arith_msg(sh_, expand_no_split(expr), "", &ok);
      if (!ok) { sh_.arith_error = true; i = end + 1; return; }
      std::string s = std::to_string(v);
      for (char c : s) { out += c; mask += qm; }
      i = end + 1;
      return;
    }
  }
  // $(cmd)
  if (n1 == '(') {
    size_t end = scan_balanced(t, i + 1, '(', ')');
    if (end != std::string::npos) {
      std::string inner = t.substr(i + 2, end - (i + 2));
      int st = 0;
      std::string res = sh_.run_and_capture(inner, &st);
      sh_.last_status = st;
      sh_.note_cmdsub(st);
      // '4' marks command-substitution output, which zsh word-splits (unlike
      // parameter expansion); double-quoted, it is not split.
      char cm = dq ? '1' : '4';
      for (char c : res) { out += c; mask += cm; }
      i = end + 1;
      return;
    }
  }
  // ${ cmd; } -- function substitution: run in the current shell, capture
  // stdout.  Distinguished from ${var} by whitespace (or `|') after the brace.
  if (n1 == '{' && i + 2 < t.size() &&
      (std::isspace(static_cast<unsigned char>(t[i + 2])) || t[i + 2] == '|')) {
    size_t end = scan_balanced(t, i + 1, '{', '}');
    if (end != std::string::npos) {
      std::string inner = t.substr(i + 2, end - (i + 2));
      if (!inner.empty() && inner[0] == '|') inner[0] = ' ';  // ${| cmd; } valsub
      int st = 0;
      std::string res = sh_.run_and_capture_inproc(inner, &st);
      sh_.last_status = st;
      sh_.note_cmdsub(st);
      char cm = dq ? '1' : '4';  // command output: zsh-splittable (see $(...) above)
      for (char c : res) { out += c; mask += cm; }
      i = end + 1;
      return;
    }
  }
  // ${...}
  if (n1 == '{') {
    size_t end = scan_balanced(t, i + 1, '{', '}');
    if (end != std::string::npos) {
      std::string body = t.substr(i + 2, end - (i + 2));

      // ${@} / ${*}: a bare positional list, identical to $@ / $*.  Handle it
      // here so `"${@}"' keeps its per-parameter field structure rather than
      // being flattened to a single word by the generic scalar path below.
      if (body == "@" || body == "*") {
        const auto &pos = sh_.positional;
        if (body[0] == '@' && dq) {
          absorb_qnull();
          for (size_t k = 0; k < pos.size(); k++) {
            if (k) { out += FIELD_SEP; mask += MMARK; }
            out += QNULL; mask += MMARK;
            for (char c : pos[k]) { out += c; mask += '1'; }
          }
        } else if (body[0] == '*' && dq) {
          std::string sep = sh_.ifs();
          std::string joiner = sep.empty() ? std::string() : std::string(1, sep[0]);
          for (size_t k = 0; k < pos.size(); k++) {
            if (k) for (char c : joiner) { out += c; mask += '1'; }
            for (char c : pos[k]) { out += c; mask += '1'; }
          }
        } else {
          for (size_t k = 0; k < pos.size(); k++) {
            if (k) { out += FIELD_SEP; mask += MMARK; }
            for (char c : pos[k]) { out += c; mask += '0'; }
          }
        }
        i = end + 1;
        return;
      }

      // ${name-word} / ${name+word} (and the `:' forms) with the operator
      // firing: expand WORD by re-processing it here, so an embedded "$@"
      // keeps its field structure (a flat string would lose empty fields).
      {
        size_t q = 0;
        std::string nm;
        if (q < body.size() &&
            (body[q] == '@' || body[q] == '*' || body[q] == '#' || body[q] == '?' ||
             body[q] == '$' || body[q] == '!' || body[q] == '-')) {
          nm = body.substr(q, 1);
          q++;
        } else {
          while (q < body.size() &&
                 (std::isalnum(static_cast<unsigned char>(body[q])) || body[q] == '_'))
            q++;
          nm = body.substr(0, q);
        }
        if (!nm.empty() && q < body.size()) {
          bool colon = body[q] == ':';
          size_t opq = q + (colon ? 1 : 0);
          if (opq < body.size() && (body[opq] == '-' || body[opq] == '+') &&
              !(nm == "-" && q == 1)) {
            char op = body[opq];
            bool set = false;
            std::string val = param_value(nm, set, true);
            bool fire = (op == '-') ? (!set || (colon && val.empty()))
                                    : (set && !(colon && val.empty()));
            if (fire) {
              std::string word = body.substr(opq + 1);
              bool has_at = word.find("$@") != std::string::npos ||
                            word.find("$*") != std::string::npos;
              // In double quotes (bash-family), expand the word in double-quote
              // context so backslash escapes and literal quotes behave right;
              // a word with "$@"/"$*" keeps its field structure via process().
              if (dq && !sh_.is_zsh() && !has_at) {
                std::string ex = expand_dq_word(word);
                for (char c : ex) { out += c; mask += '1'; }
              } else {
                // Unquoted default word: a leading `~' tilde-expands (bash).
                // In a here-document the word keeps here-document quoting
                // (e.g. $'...' stays literal), so pass the flag through.
                process(expand_leading_tilde(sh_, word), out, mask, false, heredoc);
              }
              i = end + 1;
              return;
            }
            if (op == '-' || op == '+') {
              // Operator does not fire: the parameter's own value (for `-')
              // or nothing (for `+'); fall through for arrays/subscripts.
              if (nm != "@" && nm != "*" && body.find('[') == std::string::npos) {
                if (op == '-') {
                  char qm2 = dq ? '1' : '0';
                  for (char c : val) { out += c; mask += qm2; }
                }
                i = end + 1;
                return;
              }
            }
          }
        }
      }
      // zsh `${(flags)name}' expansion flags (join/split/sort/unique/case/...).
      if (sh_.is_zsh() && !body.empty() && body[0] == '(' &&
          emit_zsh_flags(body, dq, out, mask)) {
        i = end + 1;
        return;
      }
      // zsh `${=name}': force IFS word-splitting of the value, even in quotes.
      if (sh_.is_zsh() && body.size() > 1 && body[0] == '=') {
        std::string val = expand_brace_body(*this, sh_, body.substr(1), dq);
        std::string ifs = sh_.ifs();
        auto is_ifs = [&](char c) { return ifs.find(c) != std::string::npos; };
        auto is_ws = [&](char c) { return c == ' ' || c == '\t' || c == '\n'; };
        bool first = true;
        std::string cur;
        bool have = false;
        auto flush = [&]() {
          if (!have) return;
          if (!first) { out += FIELD_SEP; mask += MMARK; }
          for (char c : cur) { out += c; mask += '0'; }
          first = false; cur.clear(); have = false;
        };
        for (size_t p = 0; p < val.size();) {
          char c = val[p];
          if (is_ifs(c)) {
            if (is_ws(c)) { flush(); while (p < val.size() && is_ifs(val[p]) && is_ws(val[p])) p++; }
            else { flush(); p++; }
            continue;
          }
          cur += c; have = true; p++;
        }
        flush();
        i = end + 1;
        return;
      }
      // zsh `${a[lo,hi]}' array range, and scalar `${s[i]}' / `${s[i,j]}'
      // character/substring selection.  A single array `${a[i]}' stays in
      // expand_brace_body (1-based) so operators like ${a[i]:-x} still work; we
      // only intercept a bare name[..] with no trailing operator.
      if (sh_.is_zsh() && !body.empty() && body.back() == ']') {
        size_t lb = body.find('[');
        if (lb != std::string::npos && lb > 0) {
          std::string zn = body.substr(0, lb);
          bool ident = std::isalpha(static_cast<unsigned char>(zn[0])) || zn[0] == '_';
          for (size_t k = 1; ident && k < zn.size(); k++)
            ident = std::isalnum(static_cast<unsigned char>(zn[k])) || zn[k] == '_';
          std::string zsub = body.substr(lb + 1, body.size() - lb - 2);
          bool arr = sh_.is_array(zn);
          bool range = zsub.find(',') != std::string::npos;
          // array range, or any scalar subscript (single char or substring)
          if (ident && ((arr && range) || (!arr && sh_.is_set(zn)))) {
            emit_zsh_subscript(zn, zsub, dq, out, mask);
            i = end + 1;
            return;
          }
        }
      }
      char lead, sel;
      std::string aname;
      if (array_ref(body, lead, aname, sel)) {
        if (lead == '#') {
          std::string cnt = std::to_string(sh_.array_count(aname));
          for (char c : cnt) { out += c; mask += qm; }
        } else {
          std::vector<std::string> items =
              (lead == '!') ? sh_.array_keys(aname) : sh_.array_values(aname);
          if (sel == '@' && dq) {
            absorb_qnull();
            for (size_t k = 0; k < items.size(); k++) {
              if (k) { out += FIELD_SEP; mask += MMARK; }
              out += QNULL; mask += MMARK;  // keep an empty element as a field
              for (char c : items[k]) { out += c; mask += '1'; }
            }
          } else if (sel == '*' && dq) {
            std::string is = sh_.ifs();
            std::string j = is.empty() ? std::string() : std::string(1, is[0]);
            for (size_t k = 0; k < items.size(); k++) {
              if (k) for (char c : j) { out += c; mask += '1'; }
              for (char c : items[k]) { out += c; mask += '1'; }
            }
          } else {
            for (size_t k = 0; k < items.size(); k++) {
              if (k) { out += FIELD_SEP; mask += MMARK; }
              for (char c : items[k]) { out += c; mask += '0'; }
            }
          }
        }
        i = end + 1;
        return;
      }
      // ${@OP} / ${*OP}: apply a per-element operator (pattern removal #/%,
      // substitution /, case-mod ^/,, transform @) to each positional param.
      if ((body[0] == '@' || body[0] == '*') && body.size() > 1 &&
          (body[1] == '#' || body[1] == '%' || body[1] == '/' || body[1] == '^' ||
           body[1] == ',' || body[1] == '~' || body[1] == '@')) {
        char psel = body[0];
        std::string prest = body.substr(1);
        std::vector<std::string> items = sh_.positional;
        for (std::string &it : items)
          it = apply_param_op(*this, sh_, std::string(1, psel), it, true, prest, dq);
        if (psel == '*' && dq) {
          std::string is = sh_.ifs();
          std::string j = is.empty() ? std::string() : std::string(1, is[0]);
          for (size_t k = 0; k < items.size(); k++) {
            if (k) for (char c : j) { out += c; mask += '1'; }
            for (char c : items[k]) { out += c; mask += '1'; }
          }
        } else {
          char m = (psel == '@' && dq) ? '1' : '0';
          if (psel == '@' && dq) absorb_qnull();
          for (size_t k = 0; k < items.size(); k++) {
            if (k) { out += FIELD_SEP; mask += MMARK; }
            if (psel == '@' && dq) { out += QNULL; mask += MMARK; }
            for (char c : items[k]) { out += c; mask += m; }
          }
        }
        i = end + 1;
        return;
      }
      // ${a[@]OP} / ${a[*]OP}: apply OP to each element.
      std::string aoname, arest;
      char asel;
      if (array_op_ref(body, aoname, asel, arest)) {
        std::vector<std::string> items = sh_.array_values(aoname);
        for (std::string &it : items) it = apply_param_op(*this, sh_, aoname, it, true, arest, dq);
        if (asel == '*' && dq) {
          std::string is = sh_.ifs();
          std::string j = is.empty() ? std::string() : std::string(1, is[0]);
          for (size_t k = 0; k < items.size(); k++) {
            if (k) for (char c : j) { out += c; mask += '1'; }
            for (char c : items[k]) { out += c; mask += '1'; }
          }
        } else {
          char m = (asel == '@' && dq) ? '1' : '0';
          if (asel == '@' && dq) absorb_qnull();
          for (size_t k = 0; k < items.size(); k++) {
            if (k) { out += FIELD_SEP; mask += MMARK; }
            if (asel == '@' && dq) { out += QNULL; mask += MMARK; }  // keep empty element
            for (char c : items[k]) { out += c; mask += m; }
          }
        }
        i = end + 1;
        return;
      }
      // ${a[@]:off:len} / ${@:off:len}: array/positional slice.
      std::string slname, soffx, slenx; char ssel; bool shaslen = false;
      if (slice_ref(body, slname, ssel, soffx, slenx, shaslen)) {
        std::vector<std::string> list;
        if (slname.empty()) {  // positionals: index 0 is $0
          list.push_back(sh_.arg0);
          for (const auto &pp : sh_.positional) list.push_back(pp);
        } else {
          list = sh_.array_values(slname);
        }
        long long n = static_cast<long long>(list.size());
        bool ok = true;
        long long off = eval_arith(sh_, expand_no_split(soffx), &ok);
        if (!ok) off = 0;
        if (off < 0) { off += n; if (off < 0) off = 0; }
        long long count;
        if (shaslen) {
          long long len = eval_arith(sh_, expand_no_split(slenx), &ok);
          if (!ok) len = 0;
          count = (len < 0) ? (n + len - off) : len;  // negative len = offset from end
        } else {
          count = n - off;
        }
        std::vector<std::string> slice;
        for (long long k = off; k < n && static_cast<long long>(slice.size()) < count; k++)
          if (k >= 0) slice.push_back(list[static_cast<size_t>(k)]);
        if (ssel == '*' && dq) {
          std::string is = sh_.ifs();
          std::string j = is.empty() ? std::string() : std::string(1, is[0]);
          if (!slice.empty()) { out += QNULL; mask += MMARK; }  // "" stays a field
          for (size_t k = 0; k < slice.size(); k++) {
            if (k) for (char c : j) { out += c; mask += '1'; }
            for (char c : slice[k]) { out += c; mask += '1'; }
          }
        } else {
          char m = (ssel == '@' && dq) ? '1' : '0';
          if (ssel == '@' && dq) absorb_qnull();
          for (size_t k = 0; k < slice.size(); k++) {
            if (k) { out += FIELD_SEP; mask += MMARK; }
            if (ssel == '@' && dq) { out += QNULL; mask += MMARK; }  // keep empty element
            for (char c : slice[k]) { out += c; mask += m; }
          }
        }
        i = end + 1;
        return;
      }
      std::string val = expand_brace_body(*this, sh_, body, dq);
      for (char c : val) { out += c; mask += qm; }
      i = end + 1;
      return;
    }
  }
  // $name or special single char
  std::string name;
  if (std::isalpha(static_cast<unsigned char>(n1)) || n1 == '_') {
    size_t j = i + 1;
    while (j < t.size() && (std::isalnum(static_cast<unsigned char>(t[j])) || t[j] == '_')) j++;
    name = t.substr(i + 1, j - (i + 1));
    i = j;
  } else if (n1 == '@' || n1 == '*') {
    // positional list
    const auto &pos = sh_.positional;
    if (n1 == '@' && dq) {
      absorb_qnull();
      for (size_t k = 0; k < pos.size(); k++) {
        if (k) { out += FIELD_SEP; mask += MMARK; }
        out += QNULL; mask += MMARK;  // keep an empty positional as a field
        for (char c : pos[k]) { out += c; mask += '1'; }
      }
    } else if (n1 == '*' && dq) {
      std::string sep = sh_.ifs();
      std::string joiner = sep.empty() ? std::string() : std::string(1, sep[0]);
      for (size_t k = 0; k < pos.size(); k++) {
        if (k) for (char c : joiner) { out += c; mask += '1'; }
        for (char c : pos[k]) { out += c; mask += '1'; }
      }
    } else {  // unquoted $@ or $*
      for (size_t k = 0; k < pos.size(); k++) {
        if (k) { out += FIELD_SEP; mask += MMARK; }
        for (char c : pos[k]) { out += c; mask += '0'; }
      }
    }
    i += 2;
    return;
  } else if (n1 == '#' && sh_.is_zsh() && i + 2 < t.size() &&
             (std::isalpha(static_cast<unsigned char>(t[i + 2])) || t[i + 2] == '_')) {
    // zsh `$#name': element count for an array, string length for a scalar.
    size_t j = i + 2;
    while (j < t.size() && (std::isalnum(static_cast<unsigned char>(t[j])) || t[j] == '_')) j++;
    std::string nm = t.substr(i + 2, j - (i + 2));
    std::string cnt = sh_.is_array(nm) ? std::to_string(sh_.array_count(nm))
                                       : std::to_string(sh_.get(nm).size());
    for (char c : cnt) { out += c; mask += qm; }
    i = j;
    return;
  } else if (n1 == '?' || n1 == '$' || n1 == '!' || n1 == '#' || n1 == '-') {
    name = std::string(1, n1);
    i += 2;
  } else if (std::isdigit(static_cast<unsigned char>(n1))) {
    name = std::string(1, n1);
    i += 2;
  } else {
    out += '$';
    mask += qm;
    i += 1;
    return;
  }
  // zsh brace-free subscript: `$name[i]' / `$name[lo,hi]' (1-based).  On an
  // array this indexes elements; on a scalar it indexes characters (substring).
  if (sh_.is_zsh() && !name.empty() &&
      (std::isalpha(static_cast<unsigned char>(name[0])) || name[0] == '_') &&
      i < t.size() && t[i] == '[' && (sh_.is_array(name) || sh_.is_set(name))) {
    size_t s = i + 1, p = i + 1;
    int d = 1;
    while (p < t.size() && d) { if (t[p] == '[') d++; else if (t[p] == ']') d--; if (d) p++; }
    std::string zsub = t.substr(s, p - s);
    if (p < t.size() && t[p] == ']') p++;
    emit_zsh_subscript(name, zsub, dq, out, mask);
    i = p;
    return;
  }
  // zsh: a bare `$array' expands to every element, not just element 0.  Unquoted
  // it yields one word per element (like `${array[@]}'); double-quoted it joins
  // the elements with the first IFS character (like `"${array[*]}"').
  if (sh_.is_zsh() && sh_.is_array(name)) {
    std::vector<std::string> items = sh_.array_values(name);
    if (dq) {
      std::string is = sh_.ifs();
      std::string joiner = is.empty() ? std::string() : std::string(1, is[0]);
      for (size_t k = 0; k < items.size(); k++) {
        if (k) for (char c : joiner) { out += c; mask += '1'; }
        for (char c : items[k]) { out += c; mask += '1'; }
      }
    } else {
      for (size_t k = 0; k < items.size(); k++) {
        if (k) { out += FIELD_SEP; mask += MMARK; }
        for (char c : items[k]) { out += c; mask += '0'; }
      }
    }
    return;
  }
  bool set = false;
  std::string v = param_value(name, set);
  for (char c : v) { out += c; mask += qm; }
}

// Parse and apply ${...} operators.
// ${var@Q}: single-quote the value so it can be re-read as shell input, using
// $'...' when it contains control characters.
static std::string atq_quote(const std::string &s) {
  bool ctrl = false;
  for (unsigned char c : s)
    if (c < 32 || c == 127) { ctrl = true; break; }
  if (ctrl) {
    std::string r = "$'";
    for (unsigned char c : s) {
      switch (c) {
        case '\n': r += "\\n"; break;
        case '\t': r += "\\t"; break;
        case '\r': r += "\\r"; break;
        case '\\': r += "\\\\"; break;
        case '\'': r += "\\'"; break;
        default:
          if (c < 32 || c == 127) { char b[8]; std::snprintf(b, sizeof b, "\\%03o", c); r += b; }
          else r += static_cast<char>(c);
      }
    }
    return r + "'";
  }
  std::string r = "'";
  for (char c : s) {
    if (c == '\'') r += "'\\''";
    else r += c;
  }
  return r + "'";
}

// ${var@E}: interpret ANSI-C backslash escapes in the value.
static std::string ansic_expand(const std::string &s) {
  std::string out;
  for (size_t i = 0; i < s.size(); i++) {
    if (s[i] != '\\' || i + 1 >= s.size()) { out += s[i]; continue; }
    switch (s[++i]) {
      case 'n': out += '\n'; break;
      case 't': out += '\t'; break;
      case 'r': out += '\r'; break;
      case 'a': out += '\a'; break;
      case 'b': out += '\b'; break;
      case 'f': out += '\f'; break;
      case 'v': out += '\v'; break;
      case '\\': out += '\\'; break;
      case '\'': out += '\''; break;
      case '"': out += '"'; break;
      case 'e': out += '\033'; break;
      default: out += '\\'; out += s[i]; break;
    }
  }
  return out;
}

static std::string expand_brace_body(Expander &ex, Shell &sh, const std::string &body,
                                    bool dq) {
  // Leading `#' means length-of.
  bool length = false;
  std::string b = body;
  if (b.size() > 1 && b[0] == '#') { length = true; b = b.substr(1); }

  // ${!name} indirection and ${!prefix*}/${!prefix@} name listing.  A `['
  // after the name is the ${!arr[@]} keys form, handled by the array path.
  if (b.size() > 1 && b[0] == '!' &&
      (std::isalpha(static_cast<unsigned char>(b[1])) || b[1] == '_')) {
    size_t q = 1;
    while (q < b.size() && (std::isalnum(static_cast<unsigned char>(b[q])) || b[q] == '_')) q++;
    std::string iname = b.substr(1, q - 1);
    if (q == b.size() || b[q] != '[') {
      if (q + 1 == b.size() && (b[q] == '*' || b[q] == '@')) {
        std::string names;
        for (const auto &kv : sh.vars) {
          if (kv.first.compare(0, iname.size(), iname) != 0) continue;
          if (!names.empty()) names += ' ';
          names += kv.first;
        }
        return names;
      }
      // The value of INAME is the parameter to expand; any operator that
      // follows applies to the indirected parameter.
      std::string target = sh.get(iname);
      if (length) return std::to_string(expand_brace_body(ex, sh, target + b.substr(q), dq).size());
      return expand_brace_body(ex, sh, target + b.substr(q), dq);
    }
  }

  size_t p = 0;
  std::string name;
  if (p < b.size() && (b[p] == '@' || b[p] == '*' || b[p] == '?' || b[p] == '$' ||
                       b[p] == '!' || b[p] == '#' || b[p] == '-')) {
    name = b.substr(0, 1);
    p = 1;
  } else if (p < b.size() && (std::isalpha(static_cast<unsigned char>(b[p])) || b[p] == '_' ||
                              std::isdigit(static_cast<unsigned char>(b[p])))) {
    size_t s = p;
    if (std::isdigit(static_cast<unsigned char>(b[p]))) {
      while (p < b.size() && std::isdigit(static_cast<unsigned char>(b[p]))) p++;
    } else {
      while (p < b.size() && (std::isalnum(static_cast<unsigned char>(b[p])) || b[p] == '_')) p++;
    }
    name = b.substr(s, p - s);
  }

  // Optional array subscript name[sub].
  bool have_sub = false;
  std::string sub;
  if (p < b.size() && b[p] == '[') {
    size_t s = p + 1;
    int d = 1;
    p++;
    while (p < b.size() && d) {
      if (b[p] == '[') d++;
      else if (b[p] == ']') d--;
      if (d) p++;
    }
    sub = b.substr(s, p - s);
    have_sub = true;
    if (p < b.size() && b[p] == ']') p++;
  }

  // zsh: `${#a}' on an array is the element count (bash gives the length of
  // element 0); `${#a[i]}' keeps its meaning (length of that element).
  if (length && !have_sub && sh.is_zsh() && sh.is_array(name))
    return std::to_string(sh.array_count(name));

  // A defaulting/alternative/error operator (`-` `:-` `=` `:=` `+` `:+` `?` `:?`)
  // handles an unset variable itself, so it must not trip `set -u' in
  // param_value.  A bare `:' here begins a substring, not such an operator.
  std::string rest = length ? std::string() : b.substr(p);
  bool defaulting_op = false;
  if (!rest.empty()) {
    char c0 = rest[0];
    if (c0 == '-' || c0 == '=' || c0 == '+' || c0 == '?') defaulting_op = true;
    else if (c0 == ':' && rest.size() > 1 &&
             (rest[1] == '-' || rest[1] == '=' || rest[1] == '+' || rest[1] == '?'))
      defaulting_op = true;
  }

  bool set = false;
  std::string val;
  if (have_sub) {
    // zsh subscripts are 1-based; translate before the (0-based) array read.
    val = sh.array_get(name, sh.zsh_subscript(name, ex.expand_no_split(sub)));
    set = sh.is_set(name);
  } else {
    val = ex.param_value(name, set, defaulting_op);
  }
  if (length) return std::to_string(val.size());
  return apply_param_op(ex, sh, name, val, set, rest, dq);
}

// Apply the operator suffix `rest' (everything after the name/subscript) of a
// ${...} expansion to a single value.  Factored out of expand_brace_body so
// array expansions can apply it to each element of ${a[@]} / ${a[*]}.
static std::string apply_param_op(Expander &ex, Shell &sh, const std::string &name,
                                  std::string val, bool set, const std::string &rest,
                                  bool dq) {
  if (rest.empty()) return val;

  // In a double-quoted context the alternative word is expanded in double-quote
  // context so backslash escapes and literal quotes behave correctly.  Skipped
  // under the zsh personality (different quoting rules) and for words that
  // contain "$@"/"$*" (which carry field structure the dq path would flatten).
  auto expand_word = [&](const std::string &w) {
    bool has_at = w.find("$@") != std::string::npos || w.find("$*") != std::string::npos;
    if (dq && !sh.is_zsh() && !has_at) return ex.expand_dq_word(w);
    return ex.expand_no_split(w);
  };

  // ${name:-word} etc.
  char op = rest[0];
  bool colon = false;
  size_t opos = 0;
  if (op == ':' && rest.size() > 1 &&
      (rest[1] == '-' || rest[1] == '=' || rest[1] == '+' || rest[1] == '?')) {
    colon = true;
    op = rest[1];
    opos = 2;
  } else if (op == '-' || op == '=' || op == '+' || op == '?') {
    opos = 1;
  } else {
    op = '\0';
  }

  if (op == '-' || op == '=' || op == '+' || op == '?') {
    std::string word = rest.substr(opos);
    bool empty = !set || (colon && val.empty());
    if (op == '-') return empty ? expand_word(word) : val;
    if (op == '+') return empty ? std::string() : expand_word(word);
    if (op == '=') {
      if (empty) {
        std::string w = expand_word(word);
        sh.set(name, w);
        return w;
      }
      return val;
    }
    if (op == '?') {
      if (empty) {
        std::string msg = expand_word(word);
        if (msg.empty()) msg = colon ? "parameter null or not set" : "parameter not set";
        std::fprintf(stderr, "%s%s: %s\n", sh.err_prefix().c_str(), name.c_str(), msg.c_str());
        sh.exiting = true;
        sh.exit_status = 127;  // bash: a fatal ${x?} / set -u error exits with 127
        return std::string();
      }
      return val;
    }
  }

  // ${name#pat} ${name##pat} ${name%pat} ${name%%pat}
  if (rest[0] == '#' || rest[0] == '%') {
    bool longest = rest.size() > 1 && rest[1] == rest[0];
    std::string pat = ex.expand_pattern(rest.substr(longest ? 2 : 1));
    if (rest[0] == '#') {  // prefix removal
      if (longest) {
        for (size_t k = val.size(); k + 1 > 0; k--) {
          if (pat_match(pat, val.substr(0, k))) return val.substr(k);
          if (k == 0) break;
        }
      } else {
        for (size_t k = 0; k <= val.size(); k++)
          if (pat_match(pat, val.substr(0, k))) return val.substr(k);
      }
    } else {  // suffix removal
      if (longest) {
        for (size_t k = 0; k <= val.size(); k++)
          if (pat_match(pat, val.substr(k))) return val.substr(0, k);
      } else {
        for (size_t k = val.size(); k + 1 > 0; k--) {
          if (pat_match(pat, val.substr(k))) return val.substr(0, k);
          if (k == 0) break;
        }
      }
    }
    return val;
  }

  // ${name/pat/rep} ${name//pat/rep}
  if (rest[0] == '/') {
    bool global = rest.size() > 1 && rest[1] == '/';
    std::string body2 = rest.substr(global ? 2 : 1);
    size_t slash = std::string::npos;
    for (size_t k = 0; k < body2.size(); k++) {
      if (body2[k] == '\\') { k++; continue; }
      if (body2[k] == '/') { slash = k; break; }
    }
    std::string pat = ex.expand_pattern(slash == std::string::npos ? body2 : body2.substr(0, slash));
    std::string rep = slash == std::string::npos ? std::string()
                                                 : ex.expand_no_split(body2.substr(slash + 1));
    if (pat.empty()) return val;
    std::string result;
    size_t k = 0;
    bool did = false;
    while (k < val.size()) {
      size_t best = std::string::npos;
      for (size_t j = val.size(); j > k; j--) {
        if (pat_match(pat, val.substr(k, j - k))) { best = j; break; }
      }
      if (best != std::string::npos && (!did || global)) {
        result += rep;
        k = (best == k) ? k + 1 : best;  // avoid infinite loop on empty match
        did = true;
        if (!global) {
          result += val.substr(k);
          return result;
        }
      } else {
        result += val[k++];
      }
    }
    return result;
  }

  // ${name^} ${name^^} ${name,} ${name,,}  (case modification)
  if (rest[0] == '^' || rest[0] == ',') {
    bool all = rest.size() > 1 && rest[1] == rest[0];
    std::string pat = ex.expand_pattern(rest.substr(all ? 2 : 1));
    bool up = rest[0] == '^';
    std::string out;
    bool first = true;
    for (char c : val) {
      std::string cs(1, c);
      bool m = pat.empty() ? true : pat_match(pat, cs);
      char nc = c;
      if (m && (all || first))
        nc = up ? static_cast<char>(std::toupper(static_cast<unsigned char>(c)))
                : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      if (!all && first) first = false;
      out += nc;
    }
    return out;
  }

  // ${name~} ${name~~}  (case toggle: ~ the first matching char, ~~ all)
  if (rest[0] == '~') {
    bool all = rest.size() > 1 && rest[1] == '~';
    std::string pat = ex.expand_pattern(rest.substr(all ? 2 : 1));
    std::string out;
    bool first = true;
    for (char c : val) {
      std::string cs(1, c);
      bool m = pat.empty() ? true : pat_match(pat, cs);
      char nc = c;
      if (m && (all || first)) {
        unsigned char uc = static_cast<unsigned char>(c);
        if (std::isupper(uc)) nc = static_cast<char>(std::tolower(uc));
        else if (std::islower(uc)) nc = static_cast<char>(std::toupper(uc));
      }
      if (!all && first) first = false;
      out += nc;
    }
    return out;
  }

  // ${name@op} -- parameter transformations.
  if (rest[0] == '@' && rest.size() >= 2) {
    // @a/@A report the variable's attributes even when its scalar context
    // (element 0) is unset -- an assoc array without ["0"] still has them.
    if (!set && !(rest[1] == 'a' || rest[1] == 'A') )
      return std::string();  // unset -> empty (even for @Q)
    if (!set && sh.vars.find(name) == sh.vars.end()) return std::string();
    char t = rest[1];
    if (t == 'Q') return atq_quote(val);
    if (t == 'E') return ansic_expand(val);
    if (t == 'P') return expand_prompt(sh, val);
    if (t == 'U' || t == 'L' || t == 'u') {
      std::string out;
      bool first = true;
      for (char c : val) {
        unsigned char uc = static_cast<unsigned char>(c);
        if (t == 'U') out += static_cast<char>(std::toupper(uc));
        else if (t == 'L') out += static_cast<char>(std::tolower(uc));
        else { out += first ? static_cast<char>(std::toupper(uc)) : c; }
        first = false;
      }
      return out;
    }
    if (t == 'a' || t == 'A') {
      auto it = sh.vars.find(name);
      std::string flags;
      bool is_arr = false, is_assoc = false;
      if (it != sh.vars.end()) {
        const Variable &var = it->second;
        is_arr = var.kind == VarKind::Indexed;
        is_assoc = var.kind == VarKind::Assoc;
        if (is_arr) flags += 'a';
        if (is_assoc) flags += 'A';
        if (var.integer) flags += 'i';
        if (var.readonly) flags += 'r';
        if (var.exported) flags += 'x';
      }
      if (t == 'a') return flags;
      // @A: reproduce a declare/assignment statement.
      std::string q = atq_quote(val);
      if (flags.empty()) return name + "=" + q;
      return "declare -" + flags + " " + name + "=" + q;
    }
    return val;
  }

  // ${name:offset:length}
  if (rest[0] == ':') {
    std::string args = rest.substr(1);
    size_t colon2 = args.find(':');
    bool ok = true;
    long long off = eval_arith(sh, colon2 == std::string::npos ? args : args.substr(0, colon2), &ok);
    long long len = -1;
    if (colon2 != std::string::npos) len = eval_arith(sh, args.substr(colon2 + 1), &ok);
    long long n = static_cast<long long>(val.size());
    if (off < 0) off += n;
    if (off < 0) off = 0;
    if (off > n) off = n;
    std::string res = val.substr(static_cast<size_t>(off));
    if (len >= 0 && len < static_cast<long long>(res.size()))
      res = res.substr(0, static_cast<size_t>(len));
    return res;
  }

  return val;
}

void Expander::process_dq(const std::string &text, size_t &i, std::string &out,
                          std::string &mask) {
  while (i < text.size() && text[i] != '"') {
    if (text[i] == '\\' && i + 1 < text.size() &&
        (text[i + 1] == '$' || text[i + 1] == '`' || text[i + 1] == '"' ||
         text[i + 1] == '\\')) {
      out += text[i + 1];
      mask += '1';
      i += 2;
    } else if (text[i] == '$') {
      expand_dollar(text, i, true, out, mask);
    } else if (text[i] == '`') {
      size_t j = i + 1;
      std::string inner;
      while (j < text.size() && text[j] != '`') {
        if (text[j] == '\\' && j + 1 < text.size() &&
            (text[j + 1] == '`' || text[j + 1] == '\\' || text[j + 1] == '$')) {
          inner += text[j + 1];
          j += 2;
        } else {
          inner += text[j++];
        }
      }
      int st = 0;
      std::string res = sh_.run_and_capture(inner, &st);
      sh_.note_cmdsub(st);
      for (char ch : res) { out += ch; mask += '1'; }
      i = (j < text.size()) ? j + 1 : j;
    } else {
      out += text[i];
      mask += '1';
      i++;
    }
  }
}

std::string Expander::expand_dq_word(const std::string &w_in) {
  // bash recognizes $'...' ANSI-C quoting in the replacement word of
  // ${var:-word} (etc.) even when the whole expansion is double quoted, though
  // ordinary single quotes there stay literal.  Pre-decode each $'...' and
  // escape the resulting bytes so the double-quote pass below keeps them
  // literal.
  std::string w;
  for (size_t k = 0; k < w_in.size(); k++) {
    if (w_in[k] == '\\' && k + 1 < w_in.size()) { w += w_in[k]; w += w_in[k + 1]; k++; continue; }
    if (w_in[k] == '$' && k + 1 < w_in.size() && w_in[k + 1] == '\'') {
      size_t j = k + 2;
      while (j < w_in.size() && w_in[j] != '\'') { if (w_in[j] == '\\' && j + 1 < w_in.size()) j++; j++; }
      std::string decoded = ansi_c(w_in.substr(k + 2, j - (k + 2)));
      for (char c : decoded) {
        if (c == '"' || c == '\\' || c == '$' || c == '`') w += '\\';
        w += c;
      }
      k = j;  // the loop's ++ steps past the closing quote
      continue;
    }
    w += w_in[k];
  }
  // A synthetic leading quote starts the double-quote span; embedded quotes in
  // W toggle context normally (an unterminated span at the end is fine).
  std::string out, mask;
  process('"' + w, out, mask, false);
  std::string joined;
  for (size_t k = 0; k < out.size(); k++) {
    if (k < mask.size() && mask[k] == MMARK) {
      if (out[k] == FIELD_SEP) joined += ' ';
      continue;
    }
    joined += out[k];
  }
  return joined;
}

void Expander::process(const std::string &text, std::string &out, std::string &mask,
                       bool /*assignment_rhs*/, bool heredoc) {
  // Most output is about as long as the input; reserve to avoid reallocating
  // (and memmoving) the out/mask pair as they grow char by char.
  out.reserve(out.size() + text.size());
  mask.reserve(mask.size() + text.size());
  size_t i = 0;
  while (i < text.size()) {
    char c = text[i];
    if (heredoc && (c == '\'' || c == '"')) {
      // Inside a here-document, quote characters are ordinary text.
      out += c; mask += '2'; i++;
    } else if (c == '\'') {
      out += QNULL; mask += MMARK;  // a quote region yields a field even if empty
      i++;
      while (i < text.size() && text[i] != '\'') { out += text[i]; mask += '1'; i++; }
      if (i < text.size()) i++;
    } else if (c == '"') {
      out += QNULL; mask += MMARK;
      i++;
      process_dq(text, i, out, mask);
      if (i < text.size()) i++;
    } else if (!heredoc && c == '$' && i + 1 < text.size() && text[i + 1] == '"') {
      i++;  // $"...": locale-translated string; treated as a plain "..."
    } else if (!heredoc && c == '$' && i + 1 < text.size() && text[i + 1] == '\'') {
      out += QNULL; mask += MMARK;
      size_t j = i + 2;
      std::string inner;
      while (j < text.size() && text[j] != '\'') {
        if (text[j] == '\\' && j + 1 < text.size()) { inner += text[j]; inner += text[j + 1]; j += 2; }
        else inner += text[j++];
      }
      std::string dec = ansi_c(inner);
      for (char ch : dec) { out += ch; mask += '1'; }
      i = (j < text.size()) ? j + 1 : j;
    } else if (c == '\\') {
      if (heredoc) {
        // In a here-document, a backslash escapes only $, `, \, and newline;
        // before anything else it is a literal character.
        char nx = (i + 1 < text.size()) ? text[i + 1] : '\0';
        if (nx == '$' || nx == '`' || nx == '\\') { out += nx; mask += '2'; i += 2; }
        else if (nx == '\n') { i += 2; }  // line continuation
        else { out += c; mask += '2'; i++; }
      } else if (i + 1 < text.size()) { out += text[i + 1]; mask += '1'; i += 2; }
      else i++;
    } else if (c == '$') {
      expand_dollar(text, i, false, out, mask, heredoc);
    } else if (c == '`') {
      size_t j = i + 1;
      std::string inner;
      while (j < text.size() && text[j] != '`') {
        if (text[j] == '\\' && j + 1 < text.size() &&
            (text[j + 1] == '`' || text[j + 1] == '\\' || text[j + 1] == '$'))
          { inner += text[j + 1]; j += 2; }
        else
          inner += text[j++];
      }
      int st = 0;
      std::string res = sh_.run_and_capture(inner, &st);
      sh_.note_cmdsub(st);
      for (char ch : res) { out += ch; mask += '4'; }  // command output: zsh-splittable
      i = (j < text.size()) ? j + 1 : j;
    } else {
      // Literal (unquoted, not from expansion): mask '2' -- glob-active like an
      // unquoted char, but NOT subject to IFS word-splitting (only expansion
      // output, mask '0', is split).
      out += c;
      mask += '2';
      i++;
    }
  }
}

std::vector<std::pair<std::string, std::string>> Expander::split_ifs(const std::string &s,
                                                                     const std::string &mask) {
  std::string ifs = sh_.ifs();
  // Mask legend for IFS splitting:
  //   '0' parameter/array/positional expansion output -- IFS-split in bash, but
  //       NOT in zsh (zsh leaves `$var' and array elements un-split);
  //   '4' command-substitution output ($(...)/`...`) -- IFS-split in both bash
  //       and zsh (zsh word-splits command substitution even with the default
  //       SH_WORD_SPLIT off);
  //   '1' quoted, '2' literal -- never IFS-split.
  // FIELD_SEP (array/`$@' element boundary) is always a hard split for any
  // unquoted expansion output, in either shell.
  bool zsh = sh_.is_zsh();
  auto splittable = [&](char m) { return m == '4' || (m == '0' && !zsh); };
  std::vector<std::pair<std::string, std::string>> fields;
  auto is_ifs = [&](char c) { return ifs.find(c) != std::string::npos; };
  auto is_ws = [&](char c) { return c == ' ' || c == '\t' || c == '\n'; };
  auto soft_ifs = [&](size_t i) { return splittable(mask[i]) && is_ifs(s[i]); };
  auto soft_ws = [&](size_t i) { return soft_ifs(i) && is_ws(s[i]); };
  // bash's list_string algorithm (lib/sh/split.c): leading IFS whitespace is
  // skipped, then each iteration extracts one field and consumes a single
  // delimiter of the form [IFS ws]* [one IFS non-ws]? [IFS ws]*.  This makes
  // ` :' (whitespace then a non-whitespace IFS char) a single delimiter, so
  // `IFS=": "; set -- $x' on x="a :" yields just "a" rather than "a" plus a
  // spurious empty field.  Quoted ('1') and literal ('2') text is never a
  // delimiter, even when it contains IFS characters.  FIELD_SEP marks a hard
  // array/`$@' element boundary that always splits (empty elements preserved).
  size_t n = s.size(), i = 0;
  while (i < n && soft_ws(i)) i++;  // strip leading IFS whitespace
  while (i < n) {
    std::string cur, curm;
    while (i < n && !soft_ifs(i) && !(mask[i] == MMARK && s[i] == FIELD_SEP)) {
      cur += s[i];
      curm += mask[i];
      i++;
    }
    fields.emplace_back(cur, curm);
    if (i >= n) break;
    if (mask[i] == MMARK && s[i] == FIELD_SEP) {
      i++;  // hard boundary; skip any IFS whitespace leading the next element
      while (i < n && soft_ws(i)) i++;
      continue;
    }
    // Soft IFS delimiter: [ws]* [one non-ws]? [ws]*.
    while (i < n && soft_ws(i)) i++;
    if (i < n && soft_ifs(i) && !is_ws(s[i])) {
      i++;
      while (i < n && soft_ws(i)) i++;
    }
  }
  return fields;
}

std::vector<std::string> Expander::glob_field(const std::string &field, const std::string &mask) {
  // Build a glob pattern: unquoted metacharacters stay special, quoted ones are
  // backslash-escaped; also produce the literal (quotes already removed).
  std::string pattern;
  bool magic = false;
  // Under `shopt -s extglob', the operators +( !( @( *( ?( also make a word a
  // pattern (the `*('/`?(' forms are already caught by the `*'/`?' below).
  auto eg = sh_.shopt_opts.find("extglob");
  bool extglob = eg != sh_.shopt_opts.end() && eg->second;
  for (size_t i = 0; i < field.size(); i++) {
    char c = field[i];
    bool q = mask[i] == '1';
    if (!q && (c == '*' || c == '?' || c == '[')) magic = true;
    if (!q && extglob && (c == '+' || c == '!' || c == '@' || c == '*' || c == '?') &&
        i + 1 < field.size() && field[i + 1] == '(')
      magic = true;
    if (q && (c == '*' || c == '?' || c == '[' || c == '\\' || c == ']')) pattern += '\\';
    pattern += c;
  }
  if (sh_.opt_noglob || !magic) return {field};
  int gflags = 0;
  // `**' recursive globbing is on under `shopt -s globstar', and -- because zsh
  // enables it by default -- whenever the personality is zsh or the `zsh_globbing'
  // variable is set to a non-null value.
  auto gs = sh_.shopt_opts.find("globstar");
  bool globstar = (gs != sh_.shopt_opts.end() && gs->second) || sh_.is_zsh() ||
                  !sh_.get("zsh_globbing").empty();
  if (globstar) gflags |= GX_GLOBSTAR;
  // `shopt -s dotglob': a leading `.' is matched by ordinary patterns too
  // (except the `.'/`..' entries, which are always skipped).
  auto dg = sh_.shopt_opts.find("dotglob");
  if (dg != sh_.shopt_opts.end() && dg->second) gflags |= GX_MATCHDOT;
  // `shopt -u globskipdots' lets `.' and `..' be matched.
  auto gsd = sh_.shopt_opts.find("globskipdots");
  if (gsd != sh_.shopt_opts.end() && !gsd->second) gflags |= GX_NODOTSKIP;
  // A non-null $GLOBIGNORE also enables dot matching, then filters out any
  // result matching one of its colon-separated patterns.
  std::string globignore = sh_.get("GLOBIGNORE");
  if (!globignore.empty()) gflags |= GX_MATCHDOT;
  auto matches = gnash::glob::glob(pattern, gflags);
  if (!globignore.empty()) {
    // Split on `:' but not inside a bracket expression -- a `[:class:]' has
    // its own colons.
    std::vector<std::string> pats;
    std::string cur;
    int bracket = 0;
    for (char c : globignore) {
      if (c == '[') bracket++;
      else if (c == ']' && bracket > 0) bracket--;
      if (c == ':' && bracket == 0) { pats.push_back(cur); cur.clear(); }
      else cur += c;
    }
    pats.push_back(cur);
    matches.erase(std::remove_if(matches.begin(), matches.end(),
                                 [&](const std::string &m) {
                                   std::string base = m.substr(m.rfind('/') + 1);
                                   for (const std::string &gp : pats)
                                     if (!gp.empty() && pat_match(gp, base)) return true;
                                   return false;
                                 }),
                  matches.end());
  }
  if (matches.empty()) {
    auto it = sh_.shopt_opts.find("nullglob");
    if (it != sh_.shopt_opts.end() && it->second) return {};  // nullglob: remove word
    return {field};  // default: keep the pattern literally
  }
  return matches;
}

namespace {
// Fork CMD connected to a pipe and return the /dev/fd path the consumer opens.
// input==true is <(cmd): the child's stdout feeds the pipe (parent reads);
// input==false is >(cmd): the child's stdin comes from the pipe (parent writes).
std::string spawn_procsub(Shell &sh, const std::string &cmd, bool input) {
  int fds[2];
  if (pipe(fds) != 0) return std::string();
  pid_t pid = fork();
  if (pid == 0) {
    if (input) { close(fds[0]); dup2(fds[1], STDOUT_FILENO); close(fds[1]); }
    else       { close(fds[1]); dup2(fds[0], STDIN_FILENO);  close(fds[0]); }
    sh.job_control = false;
    sh.subshell_level++;
    int st = sh.run_string(cmd);
    std::fflush(nullptr);
    _exit(st & 0xff);
  }
  int keep = input ? fds[0] : fds[1];
  close(input ? fds[1] : fds[0]);
  if (pid < 0) { close(keep); return std::string(); }
  sh.procsubs.push_back({static_cast<long>(pid), keep});
  return "/dev/fd/" + std::to_string(keep);
}
}  // namespace

void Expander::extract_procsubs(std::string &word) {
  for (size_t i = 0; i + 1 < word.size();) {
    char c = word[i];
    if (c == '\\') { i += 2; continue; }
    if (c == '\'') { i++; while (i < word.size() && word[i] != '\'') i++; if (i < word.size()) i++; continue; }
    if (c == '"') { i++; while (i < word.size() && word[i] != '"') { if (word[i] == '\\') i++; i++; } if (i < word.size()) i++; continue; }
    if ((c == '<' || c == '>') && word[i + 1] == '(') {
      int depth = 0;
      size_t j = i + 1;
      for (; j < word.size(); j++) {
        if (word[j] == '(') depth++;
        else if (word[j] == ')') { if (--depth == 0) break; }
      }
      if (j >= word.size()) { i++; continue; }  // unbalanced: leave alone
      std::string cmd = word.substr(i + 2, j - (i + 2));
      std::string path = spawn_procsub(sh_, cmd, c == '<');
      if (path.empty()) { i = j + 1; continue; }
      word = word.substr(0, i) + path + word.substr(j + 1);
      i += path.size();
    } else {
      i++;
    }
  }
}

static std::string tilde_assign(Shell &sh, const std::string &text);

std::vector<std::string> Expander::expand_args(const std::vector<Word> &words) {
  std::vector<std::string> result;
  for (const Word &w : words) {
    for (const std::string &braced : brace_expand(w.text)) {
      // A word shaped like an assignment (name=value) gets assignment-style
      // tilde expansion -- after the `=' and after each `:' -- unless posix
      // mode is on.  (bash's W_ASSIGNMENT tilde rule.)
      std::string pre = braced;
      if (!sh_.opt_posix) {
        size_t q = 0;
        while (q < pre.size() &&
               (std::isalnum(static_cast<unsigned char>(pre[q])) || pre[q] == '_'))
          q++;
        if (q > 0 && q < pre.size() && pre[q] == '=' &&
            std::isalpha(static_cast<unsigned char>(pre[0])))
          pre = pre.substr(0, q + 1) + tilde_assign(sh_, pre.substr(q + 1));
      }
      std::string tilded = expand_leading_tilde(sh_, pre);
      extract_procsubs(tilded);  // <(cmd) / >(cmd) -> /dev/fd/N
      std::string out, mask;
      process(tilded, out, mask, false);
      auto fields = split_ifs(out, mask);
      // Strip quoted-null markers; a field that held only a marker survives as
      // an empty field (so "" / "$empty" yield one empty argument).
      for (auto &fm : fields) {
        std::string v, m;
        v.reserve(fm.first.size());
        m.reserve(fm.first.size());
        for (size_t k = 0; k < fm.first.size(); k++)
          if (!(fm.first[k] == QNULL && fm.second[k] == MMARK)) {
            v += fm.first[k];
            m += fm.second[k];
          }
        fm.first = std::move(v);
        fm.second = std::move(m);
      }
      for (const auto &fm : fields)
        for (const std::string &g : glob_field(fm.first, fm.second)) result.push_back(g);
    }
  }
  return result;
}

std::string Expander::expand_pattern(const std::string &text) {
  std::string src = expand_leading_tilde(sh_, text);  // `case ~ in ~)' matches
  extract_procsubs(src);
  std::string out, mask;
  process(src, out, mask, false);
  std::string r;
  r.reserve(out.size());
  for (size_t i = 0; i < out.size(); i++) {
    char c = out[i];
    if (i < mask.size() && mask[i] == MMARK) {
      if (c == FIELD_SEP) r += ' ';
      continue;  // marker bytes never reach the pattern
    }
    if (i < mask.size() && mask[i] == '1') r += '\\';  // quoted: match literally
    r += c;
  }
  return r;
}

std::string Expander::expand_no_split(const std::string &text, bool do_glob) {
  std::string src = expand_leading_tilde(sh_, text);  // case subjects, redirects
  extract_procsubs(src);  // e.g. a redirect target: < <(cmd)
  std::string out, mask;
  process(src, out, mask, false);
  // drop internal markers: field separators become spaces, quoted-nulls vanish
  std::string joined;
  joined.reserve(out.size());
  for (size_t k = 0; k < out.size(); k++) {
    if (k < mask.size() && mask[k] == MMARK) {
      if (out[k] == FIELD_SEP) joined += ' ';
      continue;
    }
    joined += out[k];
  }
  if (do_glob) {
    std::string fmask(joined.size(), '0');
    auto g = glob_field(joined, fmask);
    if (g.size() == 1) return g[0];
  }
  return joined;
}

std::string Expander::expand_assignment(const std::string &text) {
  return expand_no_split(tilde_assign(sh_, text));
}

std::string Expander::expand_heredoc(const std::string &text) {
  std::string out, mask;
  process(text, out, mask, false, /*heredoc=*/true);  // quotes stay literal
  std::string joined;
  for (size_t k = 0; k < out.size(); k++) {
    if (k < mask.size() && mask[k] == MMARK) {
      if (out[k] == FIELD_SEP) joined += ' ';
      continue;
    }
    joined += out[k];
  }
  return joined;
}

// ---- brace expansion ------------------------------------------------------

// Upper bound on the number of fields a single brace expansion may produce.
// Far above any real use ({1..1000000} still expands exactly as in bash), but
// low enough that a pathological range or combinatorial product cannot exhaust
// memory and abort the shell.
constexpr std::size_t kMaxBraceItems = 1000000;

std::vector<std::string> brace_expand(const std::string &text) {
  // Find the first top-level {...} containing a comma or a ..range.
  size_t open = std::string::npos;
  int depth = 0;
  for (size_t i = 0; i < text.size(); i++) {
    char c = text[i];
    if (c == '\\') { i++; continue; }
    if (c == '\'' ) { while (++i < text.size() && text[i] != '\'') {} continue; }
    if (c == '"') { while (++i < text.size() && text[i] != '"') { if (text[i]=='\\') i++; } continue; }
    // A `...` command substitution is opaque to outer brace expansion: its
    // `{'/`,' belong to the nested command, not this word.
    if (c == '`') { while (++i < text.size() && text[i] != '`') { if (text[i]=='\\') i++; } continue; }
    // Skip $-constructs so their `{'/`,' aren't treated as brace expansion.
    if (c == '$' && i + 1 < text.size() && (text[i + 1] == '{' || text[i + 1] == '(')) {
      char oc = text[i + 1], cc = oc == '{' ? '}' : ')';
      size_t m = scan_balanced(text, i + 1, oc, cc);
      i = (m == std::string::npos) ? text.size() : m;
      continue;
    }
    if (c == '{') { if (depth == 0) open = i; depth++; }
    else if (c == '}') {
      if (depth > 0) {
        depth--;
        if (depth == 0 && open != std::string::npos) {
          std::string inside = text.substr(open + 1, i - open - 1);
          // split on top-level commas
          std::vector<std::string> parts;
          int d = 0;
          std::string cur;
          bool comma = false;
          for (size_t k = 0; k < inside.size(); k++) {
            char ic = inside[k];
            // A backslash escapes the next character (`{abc\,def}' is a single
            // item, not two): keep both so later quote removal strips the `\'.
            if (ic == '\\' && k + 1 < inside.size()) { cur += ic; cur += inside[++k]; continue; }
            // A quoted comma is not a separator (`{"x,x"}' is one item): copy the
            // quoted span verbatim, leaving the quotes for later removal.
            if (ic == '\'') {
              cur += ic;
              while (++k < inside.size() && inside[k] != '\'') cur += inside[k];
              if (k < inside.size()) cur += inside[k];
              continue;
            }
            if (ic == '"') {
              cur += ic;
              while (++k < inside.size() && inside[k] != '"') {
                if (inside[k] == '\\' && k + 1 < inside.size()) cur += inside[k++];
                cur += inside[k];
              }
              if (k < inside.size()) cur += inside[k];
              continue;
            }
            if (ic == '{') d++;
            else if (ic == '}') d--;
            if (ic == ',' && d == 0) { parts.push_back(cur); cur.clear(); comma = true; }
            else cur += ic;
          }
          parts.push_back(cur);
          std::vector<std::string> items;
          if (comma) {
            items = parts;
          } else {
            // Sequence range {start..end} or {start..end..step}.  The step is
            // taken as a magnitude (its sign is ignored; direction runs from
            // start to end); a 0 or absent step means 1.
            size_t d1 = inside.find("..");
            if (d1 != std::string::npos) {
              std::string a = inside.substr(0, d1);
              std::string rest = inside.substr(d1 + 2);
              size_t d2 = rest.find("..");
              std::string b = (d2 == std::string::npos) ? rest : rest.substr(0, d2);
              std::string stepstr = (d2 == std::string::npos) ? std::string() : rest.substr(d2 + 2);
              char *ea = nullptr, *eb = nullptr, *es = nullptr;
              long va = std::strtol(a.c_str(), &ea, 10), vb = std::strtol(b.c_str(), &eb, 10);
              long step = stepstr.empty() ? 1 : std::strtol(stepstr.c_str(), &es, 10);
              bool step_ok = stepstr.empty() || (es && *es == '\0');
              if (step == 0) step = 1;
              else if (step < 0) step = -step;
              bool a_num = ea && *ea == '\0' && !a.empty();
              bool b_num = eb && *eb == '\0' && !b.empty();
              if (a_num && b_num && step_ok) {
                // Zero-pad the terms to a common width when either bound is
                // written with a leading zero (`{00..10}' -> 00 01 ... 10).
                auto digits = [](const std::string &s) {
                  size_t p = (!s.empty() && (s[0] == '-' || s[0] == '+')) ? 1 : 0;
                  return s.substr(p);
                };
                std::string da = digits(a), db = digits(b);
                bool pad = (da.size() > 1 && da[0] == '0') || (db.size() > 1 && db[0] == '0');
                size_t width = std::max(da.size(), db.size());
                auto fmt = [&](long v) {
                  if (!pad) return std::to_string(v);
                  bool neg = v < 0;
                  std::string d = std::to_string(neg ? -v : v);
                  while (d.size() < width) d = "0" + d;
                  return (neg ? "-" : "") + d;
                };
                unsigned long long span =
                    (va <= vb) ? static_cast<unsigned long long>(vb) - static_cast<unsigned long long>(va)
                               : static_cast<unsigned long long>(va) - static_cast<unsigned long long>(vb);
                if (span / static_cast<unsigned long long>(step) < kMaxBraceItems) {
                  if (va <= vb) for (long v = va; v <= vb; v += step) items.push_back(fmt(v));
                  else for (long v = va; v >= vb; v -= step) items.push_back(fmt(v));
                }
              } else if (a.size() == 1 && b.size() == 1 && step_ok &&
                         std::isalpha(static_cast<unsigned char>(a[0])) &&
                         std::isalpha(static_cast<unsigned char>(b[0]))) {
                char ca = a[0], cb = b[0];
                if (ca <= cb) for (int v = ca; v <= cb; v += step) items.push_back(std::string(1, static_cast<char>(v)));
                else for (int v = ca; v >= cb; v -= step) items.push_back(std::string(1, static_cast<char>(v)));
              }
            }
          }
          if (!items.empty()) {
            std::string pre = text.substr(0, open);
            std::string post = text.substr(i + 1);
            std::vector<std::string> out;
            for (const std::string &it : items)
              for (const std::string &tail : brace_expand(it + post)) {
                // Cap the combinatorial product ({a,b}{a,b}... grows as 2^n);
                // beyond the cap, leave the word unexpanded rather than exhaust
                // memory and abort.
                if (out.size() >= kMaxBraceItems) return {text};
                out.push_back(pre + tail);
              }
            return out;
          }
          // The outer {...} is not itself a brace expression (no top-level comma
          // or valid range), so its braces are literal -- but an inner brace may
          // still expand: `a-{b{d,e}}-c' -> a-{bd}-c a-{be}-c.  Recurse on the
          // interior; if it expands, keep the literal outer braces around each
          // result and combine with the (recursively expanded) postscript.
          {
            std::vector<std::string> inner = brace_expand(inside);
            bool changed = inner.size() > 1 || (inner.size() == 1 && inner[0] != inside);
            if (changed) {
              std::string pre = text.substr(0, open);
              std::string post = text.substr(i + 1);
              std::vector<std::string> out;
              for (const std::string &ie : inner)
                for (const std::string &tail : brace_expand(post)) {
                  if (out.size() >= kMaxBraceItems) return {text};
                  out.push_back(pre + "{" + ie + "}" + tail);
                }
              return out;
            }
          }
          open = std::string::npos;
        }
      }
    }
  }
  // An unmatched `{' (never balanced by a `}') is literal, but a balanced brace
  // nested after it may still expand: `a-{bdef-{g,i}-c' -> a-{bdef-g-c
  // a-{bdef-i-c'.  Re-expand the text following the stray `{' and keep it as a
  // literal prefix.
  if (open != std::string::npos && open + 1 < text.size()) {
    std::string rest = text.substr(open + 1);
    std::vector<std::string> re = brace_expand(rest);
    if (re.size() > 1 || (re.size() == 1 && re[0] != rest)) {
      std::string pre = text.substr(0, open + 1);
      std::vector<std::string> out;
      for (const std::string &r : re) {
        if (out.size() >= kMaxBraceItems) return {text};
        out.push_back(pre + r);
      }
      return out;
    }
  }
  return {text};
}

}  // namespace gnash::core
