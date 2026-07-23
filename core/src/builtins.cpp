// Copyright (c) 2026 Brian J. Fox
// Licensed under GPLv2 with the GPLv2-AI Exception.

// builtins.cpp -- shell builtins, plus the test/[ and [[ ]] evaluators.

#include "gnash/core/builtins.hpp"

#include <algorithm>
#include <cctype>
#include <csignal>
#include <cstdio>
#include <optional>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <regex.h>
#include <set>
#include <sstream>
#include <vector>
#include <cerrno>
#include <dirent.h>
#include <sys/resource.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

#include "gnash/core/expand.hpp"
#include "readline/history.h"
#include "readline/readline.h"
#include "strmatch.h"

namespace gnash::core {

namespace {

std::string join(const std::vector<std::string> &v, size_t from) {
  std::string s;
  for (size_t i = from; i < v.size(); i++) {
    if (i > from) s += ' ';
    s += v[i];
  }
  return s;
}

// ---- echo / printf -------------------------------------------------------

// printf %b: like echo -e but also interprets octal (\nnn, \0nnn) and hex
// (\xHH) escapes, and \c which stops all further output.
std::string decode_b(const std::string &s, bool &stop) {
  std::string out;
  stop = false;
  for (size_t i = 0; i < s.size(); i++) {
    if (s[i] != '\\' || i + 1 >= s.size()) { out += s[i]; continue; }
    char e = s[++i];
    switch (e) {
      case 'n': out += '\n'; break;
      case 't': out += '\t'; break;
      case 'r': out += '\r'; break;
      case 'a': out += '\a'; break;
      case 'b': out += '\b'; break;
      case 'f': out += '\f'; break;
      case 'v': out += '\v'; break;
      case '\\': out += '\\'; break;
      case 'c': stop = true; return out;
      case 'x': {
        int v = 0, k = 0;
        while (k < 2 && i + 1 < s.size() && std::isxdigit((unsigned char)s[i + 1])) {
          char h = s[++i];
          v = v * 16 + (h <= '9' ? h - '0' : (std::tolower(h) - 'a' + 10));
          k++;
        }
        out += static_cast<char>(v);
        break;
      }
      case '0': {
        // \0nnn : the 0 is a prefix; up to 3 octal digits follow.
        int v = 0, k = 0;
        while (k < 3 && i + 1 < s.size() && s[i + 1] >= '0' && s[i + 1] <= '7') {
          v = v * 8 + (s[++i] - '0');
          k++;
        }
        out += static_cast<char>(v);
        break;
      }
      case '1': case '2': case '3':
      case '4': case '5': case '6': case '7': {
        int v = e - '0', k = 1;
        while (k < 3 && i + 1 < s.size() && s[i + 1] >= '0' && s[i + 1] <= '7') {
          v = v * 8 + (s[++i] - '0');
          k++;
        }
        out += static_cast<char>(v);
        break;
      }
      default: out += '\\'; out += e; break;
    }
  }
  return out;
}

int bi_echo(Shell &, const std::vector<std::string> &argv) {
  size_t i = 1;
  bool newline = true, escapes = false;
  // Leading options; combined flags like -ne are accepted.
  for (; i < argv.size(); i++) {
    const std::string &a = argv[i];
    if (a.size() < 2 || a[0] != '-') break;
    bool all_flags = true;
    for (size_t k = 1; k < a.size(); k++)
      if (a[k] != 'n' && a[k] != 'e' && a[k] != 'E') all_flags = false;
    if (!all_flags) break;
    for (size_t k = 1; k < a.size(); k++) {
      if (a[k] == 'n') newline = false;
      else if (a[k] == 'e') escapes = true;
      else if (a[k] == 'E') escapes = false;
    }
  }
  std::string out;
  bool stop = false;
  for (size_t j = i; j < argv.size() && !stop; j++) {
    if (j > i) out += ' ';
    out += escapes ? decode_b(argv[j], stop) : argv[j];
  }
  std::fwrite(out.data(), 1, out.size(), stdout);
  if (newline && !stop) std::fputc('\n', stdout);  // \c suppresses everything after
  return 0;
}

// ---- %q shell-quoting (matches bash's sh_backslash_quote / ansic_quote) ----

// Length of the valid UTF-8 sequence starting at s[i], or 0 if invalid.  Used
// so multibyte text stays literal under %q while stray high bytes are escaped.
size_t utf8_seq_len(const std::string &s, size_t i) {
  unsigned char c = s[i];
  size_t len;
  if (c < 0x80) return 1;
  if (c >= 0xc2 && c <= 0xdf) len = 2;
  else if (c >= 0xe0 && c <= 0xef) len = 3;
  else if (c >= 0xf0 && c <= 0xf4) len = 4;
  else return 0;
  if (i + len > s.size()) return 0;
  for (size_t k = 1; k < len; k++)
    if ((static_cast<unsigned char>(s[i + k]) & 0xc0) != 0x80) return 0;
  return len;
}

// The current locale takes multibyte (UTF-8) text.
bool locale_is_utf8() {
  const char *v = std::getenv("LC_ALL");
  if (v == nullptr || *v == '\0') v = std::getenv("LC_CTYPE");
  if (v == nullptr || *v == '\0') v = std::getenv("LANG");
  if (v == nullptr) return false;
  std::string s = v;
  return s.find("UTF-8") != std::string::npos || s.find("utf8") != std::string::npos ||
         s.find("utf-8") != std::string::npos || s.find("UTF8") != std::string::npos;
}

bool q_needs_ansic(const std::string &s) {
  bool mb = locale_is_utf8();
  for (size_t i = 0; i < s.size(); i++) {
    unsigned char c = s[i];
    if (c < 32 || c == 127) return true;
    if (c >= 128) {
      size_t len = mb ? utf8_seq_len(s, i) : 0;
      if (len == 0) return true;
      i += len - 1;
    }
  }
  return false;
}
std::string q_ansic(const std::string &s) {
  bool mb = locale_is_utf8();
  std::string r = "$'";
  for (size_t i = 0; i < s.size(); i++) {
    unsigned char c = s[i];
    switch (c) {
      case '\n': r += "\\n"; break;
      case '\t': r += "\\t"; break;
      case '\r': r += "\\r"; break;
      case '\\': r += "\\\\"; break;
      case '\'': r += "\\'"; break;
      case '\a': r += "\\a"; break;
      case '\b': r += "\\b"; break;
      case '\f': r += "\\f"; break;
      case '\v': r += "\\v"; break;
      default:
        if (c >= 128) {
          size_t len = mb ? utf8_seq_len(s, i) : 0;
          if (len > 0) {
            r.append(s, i, len);
            i += len - 1;
            break;
          }
        }
        if (c < 32 || c >= 127) {
          char b[8];
          std::snprintf(b, sizeof b, "\\%03o", c);
          r += b;
        } else {
          r += static_cast<char>(c);
        }
    }
  }
  r += '\'';
  return r;
}
std::string shell_quote(const std::string &s) {
  if (s.empty()) return "''";
  if (q_needs_ansic(s)) return q_ansic(s);
  // Characters bash leaves unescaped, plus alphanumerics; `~' and `#' are only
  // safe when they do not start the string.
  static const char *safe = "#%+-./:=@_~";
  std::string r;
  for (size_t i = 0; i < s.size(); i++) {
    unsigned char c = s[i];
    bool ok = std::isalnum(c) || (c != 0 && std::strchr(safe, c) != nullptr);
    if (ok && i == 0 && (c == '~' || c == '#')) ok = false;
    if (!ok) r += '\\';
    r += static_cast<char>(c);
  }
  return r;
}

// Decode the backslash escape starting at fmt[i] (fmt[i] == '\\') in a printf
// FORMAT string, appending the result to OUT and advancing I past the escape.
// ANSI-C escapes plus octal \N..\NNN (a leading 0 is just the first digit) and
// hex \xH[H], as bash's printf does for format strings.
void decode_fmt_escape(const std::string &fmt, size_t &i, std::string &out) {
  if (i + 1 >= fmt.size()) {
    out += '\\';
    i++;
    return;
  }
  char e = fmt[++i];
  i++;
  switch (e) {
    case 'n': out += '\n'; return;
    case 't': out += '\t'; return;
    case 'r': out += '\r'; return;
    case 'a': out += '\a'; return;
    case 'b': out += '\b'; return;
    case 'f': out += '\f'; return;
    case 'v': out += '\v'; return;
    case 'e': case 'E': out += '\033'; return;
    case '\\': out += '\\'; return;
    case '\'': out += '\''; return;
    case '"': out += '"'; return;
    case '?': out += '?'; return;
    case 'x': {
      int v = 0, k = 0;
      while (k < 2 && i < fmt.size() && std::isxdigit(static_cast<unsigned char>(fmt[i]))) {
        char h = fmt[i++];
        v = v * 16 + (h <= '9' ? h - '0' : (std::tolower(h) - 'a' + 10));
        k++;
      }
      if (k == 0) { out += "\\x"; return; }
      out += static_cast<char>(v);
      return;
    }
    case '0': case '1': case '2': case '3':
    case '4': case '5': case '6': case '7': {
      int v = e - '0', k = 1;
      while (k < 3 && i < fmt.size() && fmt[i] >= '0' && fmt[i] <= '7') {
        v = v * 8 + (fmt[i++] - '0');
        k++;
      }
      out += static_cast<char>(v);
      return;
    }
    default: out += '\\'; out += e; return;
  }
}

// Format one numeric/string conversion with width/precision via snprintf.
// Returns false if the conversion cannot be produced -- either the field width
// is too large to represent (snprintf reports < 0) or the output buffer cannot
// be allocated -- so the caller can report an error instead of letting a
// pathological width (e.g. printf '%2000000000d') abort the shell on bad_alloc.
template <typename T>
bool append_formatted(std::string &out, const std::string &spec, T value) {
  int need = std::snprintf(nullptr, 0, spec.c_str(), value);
  if (need < 0) return false;
  std::vector<char> buf;
  try {
    buf.resize(static_cast<size_t>(need) + 1);
  } catch (const std::bad_alloc &) {
    return false;
  }
  std::snprintf(buf.data(), buf.size(), spec.c_str(), value);
  out.append(buf.data(), static_cast<size_t>(need));
  return true;
}

// Apply Shell::array_expand_once_ok to a full `name[sub]' target (e.g. a `read'
// variable), rewriting the subscript to its resolved index; false (diagnostic
// printed) on rejection.
bool array_expand_once_name(Shell &sh, std::string &name) {
  size_t lb = name.find('[');
  if (lb == std::string::npos || name.empty() || name.back() != ']') return true;
  std::string base = name.substr(0, lb);
  std::string sub = name.substr(lb + 1, name.size() - lb - 2);
  if (!sh.array_expand_once_ok(base, sub)) return false;
  name = base + "[" + sub + "]";
  return true;
}

int bi_printf(Shell &sh, const std::vector<std::string> &argv) {
  // Options: -v NAME (assign to a variable), -- (end of options).
  size_t ai = 1;
  bool to_var = false;
  std::string vname;
  while (ai < argv.size() && argv[ai].size() >= 2 && argv[ai][0] == '-') {
    if (argv[ai] == "--") { ai++; break; }
    if (argv[ai] == "-v" && ai + 1 < argv.size()) {
      to_var = true;
      vname = argv[ai + 1];
      ai += 2;
      continue;
    }
    break;
  }
  if (ai >= argv.size()) return 0;
  std::string fmt = argv[ai++];
  size_t argi = ai;
  std::string out;
  bool oversize = false;  // a field width too large to represent/allocate
  auto next = [&]() -> std::string {
    return argi < argv.size() ? argv[argi++] : std::string();
  };
  bool consumed_any = true;
  do {
    consumed_any = false;
    for (size_t i = 0; i < fmt.size(); i++) {
      char c = fmt[i];
      if (c == '\\') {
        size_t p = i;
        decode_fmt_escape(fmt, p, out);
        i = p - 1;  // loop increment lands on the next character
      } else if (c == '%' && i + 1 < fmt.size()) {
        size_t j = i + 1;
        while (j < fmt.size() && std::strchr("-+ #0123456789.", fmt[j])) j++;
        if (j >= fmt.size()) { out += '%'; break; }
        char conv = fmt[j];
        std::string spec = fmt.substr(i, j - i + 1);
        if (conv == '%') {
          out += '%';
        } else if (conv == 'q') {
          out += shell_quote(next());
          consumed_any = true;
        } else if (conv == 'b') {
          bool stop = false;
          out += decode_b(next(), stop);
          consumed_any = true;
          if (stop) { argi = argv.size(); i = fmt.size(); break; }
        } else if (conv == 's') {
          if (!append_formatted(out, spec, next().c_str())) oversize = true;
          consumed_any = true;
        } else if (conv == 'c') {
          std::string a = next();
          if (!a.empty()) out += a[0];
          consumed_any = true;
        } else if (conv == 'd' || conv == 'i' || conv == 'x' || conv == 'X' ||
                   conv == 'o' || conv == 'u') {
          std::string sp = spec;
          sp.insert(sp.size() - 1, "l");  // promote to long
          std::string a = next();
          // A leading ' or " makes the value the next character's code.
          long v = (!a.empty() && (a[0] == '\'' || a[0] == '"'))
                       ? (a.size() > 1 ? static_cast<unsigned char>(a[1]) : 0)
                       : std::strtol(a.c_str(), nullptr, 0);
          if (!append_formatted(out, sp, v)) oversize = true;
          consumed_any = true;
        } else if (conv == 'f' || conv == 'F' || conv == 'g' || conv == 'G' ||
                   conv == 'e' || conv == 'E') {
          if (!append_formatted(out, spec, std::strtod(next().c_str(), nullptr))) oversize = true;
          consumed_any = true;
        } else {
          out += spec;
        }
        i = j;
      } else {
        out += c;
      }
    }
  } while (argi < argv.size() && consumed_any);

  if (oversize) {
    std::fprintf(stderr, "%sprintf: Value too large to be stored in data type\n",
                 sh.err_prefix().c_str());
    return 1;
  }

  if (to_var) {
    auto lb = vname.find('[');
    if (lb != std::string::npos && !vname.empty() && vname.back() == ']') {
      std::string base = vname.substr(0, lb);
      std::string sub = vname.substr(lb + 1, vname.size() - lb - 2);
      if (!sh.array_expand_once_ok(base, sub)) return 1;
      sh.array_set(base, sub, out);
    } else {
      sh.set(vname, out);
    }
  } else {
    std::fwrite(out.data(), 1, out.size(), stdout);
  }
  return 0;
}

// ---- test / [ ------------------------------------------------------------

bool file_test(char op, const std::string &path) {
  struct stat st;
  switch (op) {
    case 'e': return stat(path.c_str(), &st) == 0;
    case 'f': return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
    case 'd': return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
    case 'r': return access(path.c_str(), R_OK) == 0;
    case 'w': return access(path.c_str(), W_OK) == 0;
    case 'x': return access(path.c_str(), X_OK) == 0;
    case 's': return stat(path.c_str(), &st) == 0 && st.st_size > 0;
    case 'L': case 'h': return lstat(path.c_str(), &st) == 0 && S_ISLNK(st.st_mode);
    case 'p': return stat(path.c_str(), &st) == 0 && S_ISFIFO(st.st_mode);
    case 'b': return stat(path.c_str(), &st) == 0 && S_ISBLK(st.st_mode);
    case 'c': return stat(path.c_str(), &st) == 0 && S_ISCHR(st.st_mode);
    default: return false;
  }
}

bool int_cmp(const std::string &op, long a, long b) {
  if (op == "-eq") return a == b;
  if (op == "-ne") return a != b;
  if (op == "-lt") return a < b;
  if (op == "-le") return a <= b;
  if (op == "-gt") return a > b;
  if (op == "-ge") return a >= b;
  return false;
}

// Evaluate a `test' argument vector [a..b) (exclusive of trailing ] already
// removed).  Handles !, -a/-o, unary, binary; a small recursive evaluator.
struct TestEval {
  Shell &sh;
  const std::vector<std::string> &a;
  size_t i;
  size_t end;
  bool ok = true;

  bool at_end() { return i >= end; }
  const std::string &cur() { return a[i]; }

  // `test -v NAME' / `-v NAME[sub]': true if the variable (or array element) is
  // set.  Under `shopt -s array_expand_once' an un-evaluatable subscript errors.
  bool var_is_set(const std::string &arg) {
    size_t br = arg.find('[');
    if (br != std::string::npos && !arg.empty() && arg.back() == ']') {
      std::string nm = arg.substr(0, br);
      std::string sub = arg.substr(br + 1, arg.size() - br - 2);
      if (!sh.array_expand_once_ok(nm, sub)) { ok = false; return false; }
      if (sub == "@" || sub == "*") return sh.array_count(nm) > 0;
      for (const std::string &k : sh.array_keys(nm)) if (k == sub) return true;
      return false;
    }
    return sh.is_set(arg);
  }

  bool expr() { return or_expr(); }
  bool or_expr() {
    bool v = and_expr();
    while (!at_end() && cur() == "-o") { i++; v = and_expr() || v; }
    return v;
  }
  bool and_expr() {
    bool v = term();
    while (!at_end() && cur() == "-a") { i++; v = term() && v; }
    return v;
  }
  bool term() {
    if (at_end()) return false;
    if (cur() == "!") { i++; return !term(); }
    if (cur() == "(") {
      i++;
      bool v = expr();
      if (!at_end() && cur() == ")") i++;
      return v;
    }
    // unary: -X arg
    if (i + 1 < end && cur().size() == 2 && cur()[0] == '-') {
      std::string op = cur();
      char o = op[1];
      if (std::strchr("efdrwxsLhpbc", o)) { std::string p = a[i + 1]; i += 2; return file_test(o, p); }
      if (o == 'z') { std::string s = a[i + 1]; i += 2; return s.empty(); }
      if (o == 'n') { std::string s = a[i + 1]; i += 2; return !s.empty(); }
      if (o == 'v') { std::string arg = a[i + 1]; i += 2; return var_is_set(arg); }
    }
    // binary: a OP b
    if (i + 2 < end + 1 && i + 2 <= end) {
      std::string lhs = a[i];
      std::string op = (i + 1 < end) ? a[i + 1] : std::string();
      if (op == "=" || op == "==") { std::string r = a[i + 2]; i += 3; return lhs == r; }
      if (op == "!=") { std::string r = a[i + 2]; i += 3; return lhs != r; }
      if (op == "<") { std::string r = a[i + 2]; i += 3; return lhs < r; }
      if (op == ">") { std::string r = a[i + 2]; i += 3; return lhs > r; }
      if (op.size() == 3 && op[0] == '-') {
        long l = std::strtol(lhs.c_str(), nullptr, 10);
        long r = std::strtol(a[i + 2].c_str(), nullptr, 10);
        i += 3;
        return int_cmp(op, l, r);
      }
    }
    // single argument: true if non-empty
    std::string s = cur();
    i++;
    return !s.empty();
  }
};

int bi_test(Shell &sh, const std::vector<std::string> &argv, bool bracket) {
  std::vector<std::string> a(argv.begin() + 1, argv.end());
  if (bracket) {
    if (a.empty() || a.back() != "]") return 2;  // missing ]
    a.pop_back();
  }
  if (a.empty()) return 1;
  TestEval te{sh, a, 0, a.size(), true};
  bool v = te.expr();
  return v ? 0 : 1;
}

// ---- other builtins ------------------------------------------------------

// ---- cd / pwd with bash's logical ($PWD) vs physical (getcwd) paths -------

std::string phys_cwd() {
  char b[4096];
  return getcwd(b, sizeof b) ? std::string(b) : std::string();
}

// The logical working directory: $PWD when it is a usable absolute path,
// otherwise the physical directory.  Unlike /bin/pwd this preserves the
// symlinked path the user cd'd through.
std::string logical_pwd(Shell &sh) {
  std::string p = sh.get("PWD");
  if (!p.empty() && p[0] == '/') return p;
  return phys_cwd();
}

// Collapse `.' and `..' components of an absolute path textually (without
// resolving symlinks), as logical `cd' does.
std::string canon_logical(const std::string &path) {
  std::vector<std::string> comps;
  size_t i = 0;
  while (i < path.size()) {
    while (i < path.size() && path[i] == '/') i++;
    size_t j = i;
    while (j < path.size() && path[j] != '/') j++;
    std::string c = path.substr(i, j - i);
    i = j;
    if (c.empty() || c == ".") continue;
    if (c == "..") { if (!comps.empty()) comps.pop_back(); continue; }
    comps.push_back(c);
  }
  std::string r;
  for (const std::string &c : comps) { r += '/'; r += c; }
  return r.empty() ? "/" : r;
}

// Change directory, updating $OLDPWD/$PWD.  In logical (default) mode $PWD
// becomes the lexically-canonicalized path the user named; in physical mode
// (`cd -P') it becomes the resolved getcwd().
int change_dir(Shell &sh, const std::string &dir, bool physical, const char *caller = "cd") {
  if (dir.empty()) return 1;
  std::string oldpwd = logical_pwd(sh);
  std::string logical = (dir[0] == '/') ? dir : oldpwd + "/" + dir;
  logical = canon_logical(logical);
  const std::string &target = physical ? dir : logical;
  if (chdir(target.c_str()) != 0) {
    std::fprintf(stderr, "%s%s: %s: %s\n", sh.err_prefix().c_str(), caller, dir.c_str(),
                 std::strerror(errno));
    return 1;
  }
  if (!oldpwd.empty()) sh.set_exported("OLDPWD", oldpwd);
  sh.set_exported("PWD", physical ? phys_cwd() : logical);
  return 0;
}

int bi_cd(Shell &sh, const std::vector<std::string> &argv) {
  if (sh.opt_restricted) {
    std::fprintf(stderr, "%scd: restricted\n", sh.err_prefix().c_str());
    return 1;
  }
  bool physical = false;
  size_t i = 1;
  for (; i < argv.size(); i++) {
    if (argv[i] == "-L") physical = false;
    else if (argv[i] == "-P") physical = true;
    else if (argv[i] == "--") { i++; break; }
    else break;
  }
  std::string dir;
  if (i >= argv.size()) dir = sh.get("HOME");
  else if (argv[i] == "-") {
    dir = sh.get("OLDPWD");
    if (dir.empty()) { std::fprintf(stderr, "gnash: cd: OLDPWD not set\n"); return 1; }
    std::printf("%s\n", dir.c_str());
  } else {
    dir = argv[i];
  }
  if (dir.empty()) return 1;
  // CDPATH search: for a relative target that is not `.'/`..'-anchored, try each
  // CDPATH entry; on a match via a non-`.' entry, bash prints the new directory.
  bool anchored = dir[0] == '/' ||
                  (dir[0] == '.' &&
                   (dir.size() == 1 || dir[1] == '/' ||
                    (dir[1] == '.' && (dir.size() == 2 || dir[2] == '/'))));
  if (!anchored) {
    std::string cdp = sh.get("CDPATH");
    size_t start = 0;
    while (!cdp.empty() && start <= cdp.size()) {
      size_t e = cdp.find(':', start);
      std::string ent = cdp.substr(start, e == std::string::npos ? std::string::npos : e - start);
      std::string base = ent.empty() ? "." : ent;
      std::string cand = base + "/" + dir;
      struct stat sb;
      if (stat(cand.c_str(), &sb) == 0 && S_ISDIR(sb.st_mode)) {
        int r = change_dir(sh, cand, physical);
        // bash echoes the new directory when a non-empty CDPATH entry is used
        // (an empty entry means the current directory and stays quiet).
        if (r == 0 && !ent.empty()) std::printf("%s\n", logical_pwd(sh).c_str());
        return r;
      }
      if (e == std::string::npos) break;
      start = e + 1;
    }
  }
  return change_dir(sh, dir, physical);
}

int bi_pwd(Shell &sh, const std::vector<std::string> &argv) {
  bool physical = false;
  for (size_t i = 1; i < argv.size(); i++) {
    if (argv[i] == "-P") physical = true;
    else if (argv[i] == "-L") physical = false;
  }
  std::printf("%s\n", (physical ? phys_cwd() : logical_pwd(sh)).c_str());
  return 0;
}

// ---- directory stack: dirs / pushd / popd --------------------------------

// The full stack as bash presents it: current (logical) directory on top, then
// the saved entries.
}  // namespace (reopened below)
}  // namespace gnash::core
namespace gnash::core {
std::vector<std::string> Shell::dirstack() const {
  std::vector<std::string> v;
  std::string pwd = get("PWD");
  char cwd[4096];
  if (pwd.empty() && getcwd(cwd, sizeof cwd)) pwd = cwd;
  v.push_back(pwd);
  for (const std::string &d : dir_stack) v.push_back(d);
  return v;
}
namespace {
std::vector<std::string> full_dirstack(Shell &sh) { return sh.dirstack(); }

std::string tilde_abbrev(Shell &sh, const std::string &path, bool longform) {
  std::string home = sh.get("HOME");
  if (!longform && !home.empty() &&
      (path == home || (path.size() > home.size() && path.compare(0, home.size(), home) == 0 &&
                        path[home.size()] == '/')))
    return "~" + path.substr(home.size());
  return path;
}

int do_chdir(Shell &sh, const std::string &dir) { return change_dir(sh, dir, false); }

int rot_index(const std::string &spec, size_t n);  // forward

int bi_dirs(Shell &sh, const std::vector<std::string> &argv) {
  bool longform = false, oneline = false, verbose = false;
  std::string select;  // a +N / -N argument: print only that entry
  for (size_t i = 1; i < argv.size(); i++) {
    const std::string &a = argv[i];
    // A `+N' / `-N' selector (a dash/plus followed by a digit) is not an option.
    if (a.size() >= 2 && (a[0] == '+' || a[0] == '-') &&
        std::isdigit(static_cast<unsigned char>(a[1]))) {
      select = a;
      continue;
    }
    // A `-<non-digit>' is read as a bad `-N' index (invalid number); a bare
    // argument with no leading dash is an invalid option.
    if (a.empty() || a[0] != '-') {
      std::fprintf(stderr, "%sdirs: %s: invalid option\n", sh.err_prefix().c_str(), a.c_str());
      std::fprintf(stderr, "dirs: usage: dirs [-clpv] [+N] [-N]\n");
      return 2;
    }
    bool bad = false;
    for (size_t k = 1; k < a.size() && !bad; k++) {
      if (a[k] == 'c') { sh.dir_stack.clear(); return 0; }
      else if (a[k] == 'l') longform = true;
      else if (a[k] == 'p') oneline = true;
      else if (a[k] == 'v') { oneline = true; verbose = true; }
      else bad = true;
    }
    if (bad) {
      std::fprintf(stderr, "%sdirs: %s: invalid number\n", sh.err_prefix().c_str(), a.c_str());
      std::fprintf(stderr, "dirs: usage: dirs [-clpv] [+N] [-N]\n");
      return 2;
    }
  }
  std::vector<std::string> v = full_dirstack(sh);
  if (!select.empty()) {  // print a single entry
    int idx = rot_index(select, v.size());
    if (idx < 0) {
      // dirs reports the bare number (bash strips the +/- here, unlike pushd/popd).
      std::fprintf(stderr, "%sdirs: %s: directory stack index out of range\n",
                   sh.err_prefix().c_str(), select.substr(1).c_str());
      return 1;
    }
    std::string s = tilde_abbrev(sh, v[static_cast<size_t>(idx)], longform);
    if (verbose) std::printf("%2d  %s\n", idx, s.c_str());
    else std::printf("%s\n", s.c_str());
    return 0;
  }
  for (size_t i = 0; i < v.size(); i++) {
    std::string s = tilde_abbrev(sh, v[i], longform);
    if (verbose) std::printf("%2zu  %s\n", i, s.c_str());
    else if (oneline) std::printf("%s\n", s.c_str());
    else std::printf("%s%s", i ? " " : "", s.c_str());
  }
  if (!oneline && !verbose) std::putchar('\n');
  return 0;
}

// Resolve a +N / -N rotation spec against the full stack; returns index or -1.
int rot_index(const std::string &spec, size_t n) {
  if (spec.size() < 2) return -1;
  long k = std::atol(spec.c_str() + 1);
  if (spec[0] == '+') return (k >= 0 && static_cast<size_t>(k) < n) ? static_cast<int>(k) : -1;
  if (spec[0] == '-') {
    long idx = static_cast<long>(n) - 1 - k;
    return (idx >= 0 && static_cast<size_t>(idx) < n) ? static_cast<int>(idx) : -1;
  }
  return -1;
}

int bi_pushd(Shell &sh, const std::vector<std::string> &argv) {
  // A leading `-n' suppresses the directory change (only the stack is touched).
  std::vector<std::string> a(argv.begin(), argv.end());
  bool no_cd = false;
  if (a.size() > 1 && a[1] == "-n") { no_cd = true; a.erase(a.begin() + 1); }
  if (a.size() > 1 && (a[1][0] == '+' || a[1][0] == '-') && a[1].size() > 1 &&
      std::isdigit(static_cast<unsigned char>(a[1][1]))) {
    // Rotate so the Nth entry becomes the top.
    std::vector<std::string> v = full_dirstack(sh);
    int idx = rot_index(a[1], v.size());
    if (idx < 0) { std::fprintf(stderr, "%spushd: %s: directory stack index out of range\n", sh.err_prefix().c_str(), a[1].c_str()); return 1; }
    std::rotate(v.begin(), v.begin() + idx, v.end());
    if (!no_cd && change_dir(sh, v[0], false, "pushd") != 0) return 1;
    sh.dir_stack.assign(v.begin() + 1, v.end());
    return bi_dirs(sh, {"dirs"});
  }
  // A `+'/`-' argument that isn't a numeric index is a malformed rotation count.
  if (a.size() > 1 && (a[1][0] == '+' || a[1][0] == '-')) {
    std::fprintf(stderr, "%spushd: %s: invalid number\n", sh.err_prefix().c_str(),
                 a[1].c_str());
    std::fprintf(stderr, "pushd: usage: pushd [-n] [+N | -N | dir]\n");
    return 2;
  }
  if (a.size() > 1) {
    std::string old = logical_pwd(sh);
    if (no_cd) { sh.dir_stack.insert(sh.dir_stack.begin(), a[1]); return bi_dirs(sh, {"dirs"}); }
    if (change_dir(sh, a[1], false, "pushd") != 0) return 1;
    sh.dir_stack.insert(sh.dir_stack.begin(), old);
    return bi_dirs(sh, {"dirs"});
  }
  // No argument: swap the top two directories.
  if (sh.dir_stack.empty()) {
    std::fprintf(stderr, "%spushd: no other directory\n", sh.err_prefix().c_str());
    return 1;
  }
  std::string target = sh.dir_stack.front();
  std::string old = logical_pwd(sh);
  if (do_chdir(sh, target) != 0) return 1;
  sh.dir_stack.front() = old;
  return bi_dirs(sh, {"dirs"});
}

int bi_popd(Shell &sh, const std::vector<std::string> &argv) {
  if (argv.size() > 1 && (argv[1][0] == '+' || argv[1][0] == '-') && argv[1].size() > 1 &&
      std::isdigit(static_cast<unsigned char>(argv[1][1]))) {
    std::vector<std::string> v = full_dirstack(sh);
    int idx = rot_index(argv[1], v.size());
    if (idx < 0) { std::fprintf(stderr, "%spopd: %s: directory stack index out of range\n", sh.err_prefix().c_str(), argv[1].c_str()); return 1; }
    if (idx == 0) {  // removing the top: cd to the next entry
      if (sh.dir_stack.empty()) { std::fprintf(stderr, "%spopd: directory stack empty\n", sh.err_prefix().c_str()); return 1; }
      std::string target = sh.dir_stack.front();
      sh.dir_stack.erase(sh.dir_stack.begin());
      if (change_dir(sh, target, false, "popd") != 0) return 1;
    } else {
      sh.dir_stack.erase(sh.dir_stack.begin() + (idx - 1));
    }
    return bi_dirs(sh, {"dirs"});
  }
  // A `+'/`-' argument that isn't a numeric index (and isn't the `-n' option).
  if (argv.size() > 1 && (argv[1][0] == '+' || argv[1][0] == '-') && argv[1] != "-n") {
    std::fprintf(stderr, "%spopd: %s: invalid number\n", sh.err_prefix().c_str(),
                 argv[1].c_str());
    std::fprintf(stderr, "popd: usage: popd [-n] [+N | -N]\n");
    return 2;
  }
  if (sh.dir_stack.empty()) {
    std::fprintf(stderr, "%spopd: directory stack empty\n", sh.err_prefix().c_str());
    return 1;
  }
  std::string target = sh.dir_stack.front();
  sh.dir_stack.erase(sh.dir_stack.begin());
  if (change_dir(sh, target, false, "popd") != 0) return 1;
  return bi_dirs(sh, {"dirs"});
}

// ---- mapfile / readarray -------------------------------------------------

int bi_mapfile(Shell &sh, const std::vector<std::string> &argv) {
  bool strip = false, haveO = false;
  char delim = '\n';
  long count = 0, origin = 0, skip = 0;
  int fd = 0;
  bool have_u = false;
  std::string name = "MAPFILE";
  size_t i = 1;
  for (; i < argv.size(); i++) {
    const std::string &a = argv[i];
    if (a.size() < 2 || a[0] != '-' || a == "--") { if (a == "--") i++; break; }
    char opt = a[1];
    if (opt == 't') { strip = true; continue; }
    std::string val = a.size() > 2 ? a.substr(2)
                     : (i + 1 < argv.size() && std::strchr("dnOsuCc", opt) ? argv[++i] : "");
    switch (opt) {
      case 'd': delim = val.empty() ? '\0' : val[0]; break;
      case 'n': count = std::atol(val.c_str()); break;
      case 'O': origin = std::atol(val.c_str()); haveO = true; break;
      case 's': skip = std::atol(val.c_str()); break;
      case 'u': {
        char *end = nullptr;
        long f = std::strtol(val.c_str(), &end, 10);
        if (val.empty() || *end != '\0') {
          std::fprintf(stderr, "%smapfile: %s: invalid file descriptor specification\n",
                       sh.err_prefix().c_str(), val.c_str());
          return 1;
        }
        fd = static_cast<int>(f);
        have_u = true;
        break;
      }
      case 'c': case 'C': break;  // callback quantum -- accepted, not invoked
      default: break;
    }
  }
  if (i < argv.size()) name = argv[i];

  // Validate the descriptor and the target array name before reading.
  if (have_u && fcntl(fd, F_GETFD) == -1) {
    std::fprintf(stderr, "%smapfile: %d: invalid file descriptor: %s\n", sh.err_prefix().c_str(),
                 fd, std::strerror(errno));
    return 1;
  }
  if (name.empty()) {
    std::fprintf(stderr, "%smapfile: empty array variable name\n", sh.err_prefix().c_str());
    return 1;
  }
  {
    bool ok = std::isalpha(static_cast<unsigned char>(name[0])) || name[0] == '_';
    for (char c : name)
      if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_')) ok = false;
    if (!ok) {
      std::fprintf(stderr, "%smapfile: `%s': not a valid identifier\n", sh.err_prefix().c_str(),
                   name.c_str());
      return 1;
    }
  }

  std::string data;
  char buf[4096];
  ssize_t r;
  while ((r = read(fd, buf, sizeof buf)) > 0) data.append(buf, static_cast<size_t>(r));

  // Split into records, keeping the delimiter; a trailing empty record (data
  // ended exactly on a delimiter) is not counted.
  std::vector<std::string> recs;
  size_t p = 0;
  while (p < data.size()) {
    size_t q = data.find(delim, p);
    if (q == std::string::npos) { recs.push_back(data.substr(p)); break; }
    recs.push_back(data.substr(p, q - p + 1));
    p = q + 1;
  }
  if (skip > 0 && static_cast<size_t>(skip) < recs.size())
    recs.erase(recs.begin(), recs.begin() + skip);
  else if (skip > 0)
    recs.clear();
  if (count > 0 && static_cast<size_t>(count) < recs.size()) recs.resize(count);

  if (!haveO) sh.unset(name);  // default: replace the whole array
  for (size_t k = 0; k < recs.size(); k++) {
    std::string v = recs[k];
    if (strip && !v.empty() && v.back() == delim) v.pop_back();
    sh.array_set(name, std::to_string(origin + static_cast<long>(k)), v);
  }
  return 0;
}

int bi_export(Shell &sh, const std::vector<std::string> &argv) {
  bool funcs = false;
  int st = 0;
  size_t i = 1;
  // Parse leading options: `export' takes only -f/-n/-p (and `--').
  for (; i < argv.size(); i++) {
    const std::string &a = argv[i];
    if (a == "--") { i++; break; }
    if (a.size() < 2 || a[0] != '-') break;
    for (size_t k = 1; k < a.size(); k++) {
      if (a[k] == 'f') funcs = true;
      else if (a[k] == 'n' || a[k] == 'p') { /* unmodeled here / no-op */ }
      else {
        std::fprintf(stderr, "%sexport: -%c: invalid option\n", sh.err_prefix().c_str(), a[k]);
        std::fprintf(stderr, "export: usage: export [-fn] [name[=value] ...] or export -p [-f]\n");
        return 2;
      }
    }
  }
  for (; i < argv.size(); i++) {
    const std::string &a = argv[i];
    if (funcs) {
      // `export -f name': mark a function for the environment of children.  A
      // name that cannot encode as BASH_FUNC_<name>%% (it contains `=' or `/')
      // cannot be exported, even though it is a valid function name.
      if (a.find('=') != std::string::npos || a.find('/') != std::string::npos) {
        std::fprintf(stderr, "%sexport: %s: cannot export\n", sh.err_prefix().c_str(),
                     a.c_str());
        st = 1;
      } else if (sh.functions.count(a)) {
        sh.exported_functions.insert(a);
      } else {
        // Not a function: bash refuses to create an `invisible function'.
        std::fprintf(stderr, "%sexport: %s: not a function\n", sh.err_prefix().c_str(),
                     a.c_str());
        st = 1;
      }
      continue;
    }
    size_t eq = a.find('=');
    // `export' cannot target a single array element (`export a[5]'); bash reports
    // it as an invalid identifier.  (zsh's rules differ, so leave it alone.)
    size_t br = a.find('[');
    if (br != std::string::npos && (eq == std::string::npos || br < eq) && !sh.is_zsh()) {
      std::string tgt = (eq == std::string::npos) ? a : a.substr(0, eq);
      std::fprintf(stderr, "%sexport: `%s': not a valid identifier\n",
                   sh.err_prefix().c_str(), tgt.c_str());
      st = 1;
      continue;
    }
    if (eq != std::string::npos) {
      // `export name+=value' appends to the current value.
      bool append = eq > 0 && a[eq - 1] == '+';
      std::string nm = a.substr(0, append ? eq - 1 : eq);
      std::string val = a.substr(eq + 1);
      auto exv = sh.vars.find(nm);
      if (exv != sh.vars.end() && exv->second.integer) {
        bool ok = true;
        long long rhs = eval_arith(sh, val, &ok);
        long long base = append ? eval_arith(sh, sh.get(nm), &ok) : 0;
        val = std::to_string(base + rhs);
      } else if (append) {
        val = sh.get(nm) + val;
      }
      sh.set_exported(nm, val);
    } else
      sh.export_name(a);
  }
  return st;
}

int bi_unset(Shell &sh, const std::vector<std::string> &argv) {
  bool fflag = false;  // `-f': functions
  bool vflag = false;  // `-v' given explicitly: variables only, no function fallback
  bool noref = false;  // `-n': remove the nameref itself, not its target
  int ret = 0;
  size_t i = 1;
  for (; i < argv.size(); i++) {
    const std::string &a = argv[i];
    if (a == "--") { i++; break; }
    if (a.size() < 2 || a[0] != '-') break;
    for (size_t k = 1; k < a.size(); k++) {
      if (a[k] == 'f') fflag = true;
      else if (a[k] == 'v') vflag = true;
      else if (a[k] == 'n') noref = true;
      else {
        std::fprintf(stderr, "%sunset: -%c: invalid option\n", sh.err_prefix().c_str(), a[k]);
        std::fprintf(stderr, "unset: usage: unset [-f] [-v] [-n] [name ...]\n");
        return 2;
      }
    }
  }
  if (fflag && vflag) {
    std::fprintf(stderr, "%sunset: cannot simultaneously unset a function and a variable\n",
                 sh.err_prefix().c_str());
    return 2;
  }
  bool funcs = fflag;
  // A handful of shell-maintained arrays cannot be unset.
  static const std::set<std::string> kNoUnset = {"BASH_LINENO", "BASH_SOURCE",
                                                 "BASH_ARGV", "BASH_ARGC"};
  for (; i < argv.size(); i++) {
    if (funcs) { sh.functions.erase(argv[i]); continue; }
    if (kNoUnset.count(argv[i])) {
      std::fprintf(stderr, "%sunset: %s: cannot unset\n", sh.err_prefix().c_str(),
                   argv[i].c_str());
      ret = 1;
      continue;
    }
    // `unset name[sub]' removes a single array element (or the whole array for
    // a `@'/`*' subscript), not a variable literally named "name[sub]".
    size_t lb = argv[i].find('[');
    if (!noref && lb != std::string::npos && !argv[i].empty() &&
        argv[i].back() == ']') {
      std::string base = argv[i].substr(0, lb);
      std::string sub = argv[i].substr(lb + 1, argv[i].size() - lb - 2);
      std::string bd = sh.deref(base);
      auto bit = sh.vars.find(bd);
      // Unset of an element only evaluates the subscript when the array exists;
      // `unset a[SUB]' on a missing `a' is a silent no-op (no injection check).
      if (bit != sh.vars.end() && !sh.array_expand_once_ok(base, sub)) {
        ret = 1;
        continue;
      }
      if (bit != sh.vars.end() && bit->second.readonly) {
        std::fprintf(stderr, "%sunset: %s: cannot unset: readonly variable\n",
                     sh.err_prefix().c_str(), bd.c_str());
        ret = 1;
        continue;
      }
      sh.array_unset(base, sub);
      continue;
    }
    // A readonly variable cannot be unset.  Resolve through a nameref (unless
    // -n) so the target that is actually readonly is the one reported.
    std::string tgt = noref ? argv[i] : sh.deref(argv[i]);
    auto it = sh.vars.find(tgt);
    if (it != sh.vars.end() && it->second.readonly) {
      std::fprintf(stderr, "%sunset: %s: cannot unset: readonly variable\n",
                   sh.err_prefix().c_str(), tgt.c_str());
      ret = 1;
      continue;
    }
    // With neither -v nor -f, `unset NAME' unsets a variable if one exists,
    // otherwise a function of that name (bash); explicit `-v' skips the fallback.
    if (!vflag && it == sh.vars.end() && sh.functions.count(tgt)) sh.functions.erase(tgt);
    else sh.unset(argv[i], false, noref);
  }
  return ret;
}

// Quote a scalar value for `set' output: bare if it is "simple", else single
// quoted with embedded quotes escaped (matching bash).
std::string set_quote(const std::string &v) {
  // bash's sh_single_quote conventions: a lone single quote prints as \';
  // `~' and `#' only force quoting at the start of the value.
  if (v == "'") return "\\'";
  bool simple = true;  // an empty value prints bare (name=)
  for (size_t i = 0; i < v.size(); i++) {
    unsigned char c = static_cast<unsigned char>(v[i]);
    if (std::isalnum(c) || std::strchr("_-./:=+,%@^", c) != nullptr) continue;
    if ((c == '~' || c == '#') && i != 0) continue;
    simple = false;
    break;
  }
  if (simple) return v;
  std::string r = "'";
  for (char c : v) { if (c == '\'') r += "'\\''"; else r += c; }
  return r + "'";
}

std::string set_elem(const std::string &v) {  // array element form: "value"
  std::string r = "\"";
  for (char c : v) { if (c == '"' || c == '\\' || c == '$' || c == '`') r += '\\'; r += c; }
  return r + "\"";
}

// Quote a value for `declare -p': ANSI-C ($'...') for non-printable bytes,
// double quotes otherwise (escaping " \\ $ `).
std::string declare_quote(const std::string &v) {
  if (q_needs_ansic(v)) return q_ansic(v);
  std::string r = "\"";
  for (char c : v) { if (c == '"' || c == '\\' || c == '$' || c == '`') r += '\\'; r += c; }
  return r + "\"";
}

// True if KEY contains a shell metacharacter (bash's sh_contains_shell_metas),
// used to decide whether an associative-array subscript must be quoted.
static bool sub_has_metas(const std::string &s) {
  for (size_t i = 0; i < s.size(); i++) {
    switch (s[i]) {
      case ' ': case '\t': case '\n':
      case '\'': case '"': case '\\':
      case '|': case '&': case ';':
      case '(': case ')': case '<': case '>':
      case '!': case '{': case '}':
      case '*': case '[': case '?': case ']':
      case '^': case '$': case '`':
        return true;
      case '~':
        if (i == 0 || s[i - 1] == '=' || s[i - 1] == ':') return true;
        break;
      case '#':
        if (i == 0) return true;
        break;
    }
  }
  return false;
}

// Quote an associative-array subscript for `declare -p' the way bash does:
// ANSI-C ($'...') for non-printing bytes, double quotes when it holds a shell
// metacharacter or is the lone `*'/`@' selector, otherwise bare.
std::string declare_sub_quote(const std::string &k) {
  if (q_needs_ansic(k)) return q_ansic(k);
  if (!sub_has_metas(k) && k != "*" && k != "@") return k;
  std::string r = "\"";
  for (char c : k) { if (c == '"' || c == '\\' || c == '$' || c == '`') r += '\\'; r += c; }
  return r + "\"";
}

// `declare -p NAME': the attribute flags plus the reproducible assignment.
void declare_print_var(const std::string &name, const Variable &v) {
  // Attribute letters in bash's fixed order (var_attribute_string):
  // a A f i n r t x c l u.  gnash models the subset a A i n r x l u.
  std::string f;
  if (v.kind == VarKind::Indexed) f += 'a';
  if (v.kind == VarKind::Assoc) f += 'A';
  if (v.integer) f += 'i';
  if (v.nameref) f += 'n';
  if (v.readonly) f += 'r';
  if (v.exported) f += 'x';
  if (v.capcase) f += 'c';
  if (v.lcase) f += 'l';
  if (v.ucase) f += 'u';
  std::string decl = "declare -" + (f.empty() ? std::string("-") : f);
  decl += ' ' + name;
  if (v.invisible) {
    // Declared with no value (`declare -a b'): bash prints just the attributes
    // and name, no `=' / `=()'.
  } else if (v.kind == VarKind::Indexed) {
    decl += "=(";
    bool first = true;
    for (const auto &kv : v.idx) {
      if (!first) decl += ' ';
      first = false;
      decl += "[" + std::to_string(kv.first) + "]=" + declare_quote(kv.second);
    }
    decl += ")";
  } else if (v.kind == VarKind::Assoc) {
    decl += "=(";
    bool first = true;
    for (const auto &k : Shell::assoc_order(v)) {
      if (!first) decl += ' ';
      first = false;
      decl += "[" + declare_sub_quote(k) + "]=" + declare_quote(v.assoc.at(k));
    }
    // bash prints a space before the closing paren for a non-empty assoc.
    decl += first ? ")" : " )";
  } else if (!v.value.empty() || v.integer) {
    decl += "=" + declare_quote(v.value);
  }
  std::printf("%s\n", decl.c_str());
}

void set_print_var(const std::string &name, const Variable &v) {
  if (v.kind == VarKind::Indexed) {
    std::string s = name + "=(";
    bool first = true;
    for (const auto &kv : v.idx) {
      if (!first) s += ' ';
      first = false;
      s += "[" + std::to_string(kv.first) + "]=" + set_elem(kv.second);
    }
    std::printf("%s)\n", s.c_str());
  } else if (v.kind == VarKind::Assoc) {
    std::string s = name + "=(";
    bool first = true;
    for (const auto &k : Shell::assoc_order(v)) {
      if (!first) s += ' ';
      first = false;
      s += "[" + declare_sub_quote(k) + "]=" + set_elem(v.assoc.at(k));
    }
    // bash prints a trailing space before `)' for a non-empty associative array.
    std::printf("%s%s)\n", s.c_str(), first ? "" : " ");
  } else {
    std::printf("%s=%s\n", name.c_str(), set_quote(v.value).c_str());
  }
}

// Apply one `set -o NAME' / `set +o NAME'; false if NAME is unknown.
bool set_o_option(Shell &sh, const std::string &o, bool on) {
  if (o == "errexit") sh.opt_errexit = on;
  else if (o == "keyword") sh.opt_keyword = on;
  else if (o == "physical") sh.opt_physical = on;
  else if (o == "xtrace") sh.opt_xtrace = on;
  else if (o == "nounset") sh.opt_nounset = on;
  else if (o == "noglob") sh.opt_noglob = on;
  else if (o == "verbose") sh.opt_verbose = on;
  else if (o == "noexec") { if (!sh.interactive) sh.opt_noexec = on; }
  else if (o == "functrace" || o == "errtrace") sh.opt_functrace = on;
  else if (o == "pipefail") sh.opt_pipefail = on;
  else if (o == "history") { if (on) sh.enable_history(); else sh.opt_history = false; }
  else if (o == "histexpand") sh.opt_histexpand = on;
  else if (o == "posix") sh.opt_posix = on;
  else if (o == "restricted") {
    if (on) sh.opt_restricted = true;
    else return false;  // cannot clear restricted; caller reports the error
  }
  // `set -o vi'/`set -o emacs' switch the readline editing mode (they are
  // mutually exclusive); `+o' flips to the other.  The stored flags let the
  // state be reported even in a non-interactive shell, as bash does.
  else if (o == "vi") {
    if (on) rl_vi_editing_mode(0, 0); else rl_emacs_editing_mode(0, 0);
    sh.opt_vi = on; sh.opt_emacs = !on;
  }
  else if (o == "emacs") {
    if (on) rl_emacs_editing_mode(0, 0); else rl_vi_editing_mode(0, 0);
    sh.opt_emacs = on; sh.opt_vi = !on;
  }
  else if (o == "monitor") sh.opt_monitor = on;
  else if (o == "privileged") sh.opt_privileged = on;
  // Valid bash `set -o' names whose behavior is unimplemented: accept as no-ops
  // (a script may set them; erroring would diverge from bash, which knows them).
  else if (o == "allexport" || o == "braceexpand" || o == "hashall" ||
           o == "ignoreeof" || o == "interactive-comments" || o == "nolog" ||
           o == "notify" || o == "noclobber" || o == "onecmd")
    ;  // no-op
  else return false;
  return true;
}

}  // namespace (close anon: set_option_states needs external linkage for SHELLOPTS)

// The `set -o'/`set +o' option table with each option's current state.  Also
// used by Shell::dynamic_var to build $SHELLOPTS, so it lives at namespace
// scope rather than in the anonymous namespace.
std::vector<std::pair<std::string, bool>> set_option_states(Shell &sh) {
  bool i = sh.interactive;
  return {
      {"allexport", false},   {"braceexpand", true},
      {"emacs", i ? (rl_editing_mode == 1) : sh.opt_emacs},
      {"errexit", sh.opt_errexit},
      {"errtrace", sh.opt_functrace}, {"functrace", sh.opt_functrace},
      {"hashall", true},      {"histexpand", sh.opt_histexpand},
      {"history", sh.opt_history}, {"ignoreeof", false},
      {"interactive-comments", true}, {"keyword", sh.opt_keyword},
      {"monitor", sh.job_control || sh.opt_monitor}, {"noclobber", false},
      {"noexec", sh.opt_noexec}, {"noglob", sh.opt_noglob},
      {"nolog", false},       {"notify", false},
      {"nounset", sh.opt_nounset}, {"onecmd", false},
      {"physical", sh.opt_physical}, {"pipefail", sh.opt_pipefail},
      {"posix", sh.opt_posix}, {"privileged", sh.opt_privileged},
      {"verbose", sh.opt_verbose},
      {"vi", i ? (rl_editing_mode == 0) : sh.opt_vi},
      {"xtrace", sh.opt_xtrace}};
}

namespace {  // reopen the anonymous namespace for the remaining file-local helpers

int bi_set(Shell &sh, const std::vector<std::string> &argv) {
  // No arguments: list all shell variables (name=value), names sorted.
  if (argv.size() == 1) {
    for (const auto &kv : sh.vars) {  // std::map is ordered by name
      const std::string &n = kv.first;
      if (n.empty() || !(std::isalpha(static_cast<unsigned char>(n[0])) || n[0] == '_'))
        continue;  // skip special parameters like $, ?, #
      set_print_var(n, kv.second);
    }
    return 0;
  }

  size_t i = 1;
  bool positional_given = false;
  for (; i < argv.size(); i++) {
    const std::string &a = argv[i];
    if (a == "--") { i++; positional_given = true; break; }
    if (a == "-") { sh.opt_xtrace = sh.opt_verbose = false; continue; }
    if (a.size() >= 2 && (a[0] == '-' || a[0] == '+')) {
      bool on = a[0] == '-';
      for (size_t k = 1; k < a.size(); k++) {
        switch (a[k]) {
          case 'e': sh.opt_errexit = on; break;
          case 'k': sh.opt_keyword = on; break;  // keyword: assignments anywhere
          case 'P': sh.opt_physical = on; break;  // physical: resolve symlinks
          case 'x': sh.opt_xtrace = on; break;
          case 'u': sh.opt_nounset = on; break;
          case 'f': sh.opt_noglob = on; break;
          case 'v': sh.opt_verbose = on; break;
          case 'n': if (!sh.interactive) sh.opt_noexec = on; break;  // ignored when interactive
          case 'T': sh.opt_functrace = on; break;  // DEBUG/RETURN trap inheritance
          case 'E': sh.opt_functrace = on; break;  // errtrace: ERR trap inheritance
          case 'H': sh.opt_histexpand = on; break;  // `!' history expansion
          case 'm': sh.opt_monitor = on; break;     // monitor: job control
          case 'p': sh.opt_privileged = on; break;  // privileged mode
          case 'r':  // restricted: can be turned on, never off
            if (on) sh.opt_restricted = true;
            else {
              std::fprintf(stderr, "%sset: +r: invalid option\n", sh.err_prefix().c_str());
              std::fprintf(stderr, "set: usage: set [-abefhkmnptuvxBCEHPT] [-o option-name] "
                                   "[--] [-] [arg ...]\n");
              return 2;
            }
            break;
          case 'o': {
            if (i + 1 >= argv.size()) {
              // `set -o' lists states; `set +o' reproduces as commands.
              for (const auto &o : set_option_states(sh)) {
                if (on) std::printf("%-15s\t%s\n", o.first.c_str(), o.second ? "on" : "off");
                else std::printf("set %co %s\n", o.second ? '-' : '+', o.first.c_str());
              }
            } else {
              std::string oname = argv[++i];
              if (!set_o_option(sh, oname, on)) {
                std::fprintf(stderr, "%sset: %s: invalid option name\n",
                             sh.err_prefix().c_str(), oname.c_str());
                return 2;
              }
            }
            k = a.size();  // -o consumes the rest of the word
            break;
          }
          // Flags accepted as no-ops where the behavior is unimplemented:
          // allexport/notify/hashall/onecmd/braceexpand/noclobber.
          case 'a': case 'b': case 'h': case 't': case 'B': case 'C': break;
          default:
            std::fprintf(stderr, "%sset: %c%c: invalid option\n", sh.err_prefix().c_str(),
                         a[0], a[k]);
            std::fprintf(stderr, "set: usage: set [-abefhkmnptuvxBCEHPT] [-o option-name] "
                                 "[--] [-] [arg ...]\n");
            return 2;
        }
      }
    } else {
      positional_given = true;
      break;
    }
  }
  if (positional_given) sh.positional.assign(argv.begin() + static_cast<long>(i), argv.end());
  return 0;
}

int bi_read(Shell &sh, const std::vector<std::string> &argv) {
  bool raw = false;                 // -r: don't process backslashes
  std::string arrayname;            // -a NAME: read words into the array NAME
  std::string prompt;               // -p PROMPT
  int fd = 0;                       // -u FD
  bool have_u = false;              // -u given (validate the descriptor)
  int delim = '\n';                 // -d DELIM (default newline; -d '' is NUL)
  bool have_n = false, exact = false;  // -n / -N
  long nchars = 0;
  bool have_t = false;              // -t TIMEOUT (fractional seconds)
  double timeout = 0.0;
  bool edit = false;                // -e: read with readline
  std::vector<std::string> names;

  for (size_t i = 1; i < argv.size(); i++) {
    const std::string &a = argv[i];
    if (a == "--") { for (size_t j = i + 1; j < argv.size(); j++) names.push_back(argv[j]); break; }
    if (a.size() >= 2 && a[0] == '-' && a != "-") {
      for (size_t k = 1; k < a.size(); k++) {
        char o = a[k];
        auto val = [&]() -> std::string {       // option argument (rest of word or next word)
          if (k + 1 < a.size()) { std::string v = a.substr(k + 1); k = a.size(); return v; }
          return (i + 1 < argv.size()) ? argv[++i] : std::string();
        };
        switch (o) {
          case 'r': raw = true; break;
          case 'a': arrayname = val(); break;
          case 'p': prompt = val(); break;
          case 'u': {
            std::string v = val();
            char *end = nullptr;
            long f = std::strtol(v.c_str(), &end, 10);
            if (v.empty() || *end != '\0') {
              std::fprintf(stderr, "%sread: %s: invalid file descriptor specification\n",
                           sh.err_prefix().c_str(), v.c_str());
              return 1;
            }
            fd = static_cast<int>(f);
            have_u = true;
            break;
          }
          case 'd': { std::string d = val(); delim = d.empty() ? 0 : static_cast<unsigned char>(d[0]); break; }
          case 'n': case 'N': {
            std::string v = val();
            char *end = nullptr;
            long nv = std::strtol(v.c_str(), &end, 10);
            if (v.empty() || (end && *end) || nv < 0) {
              std::fprintf(stderr, "%sread: %s: invalid number\n", sh.err_prefix().c_str(),
                           v.c_str());
              return 1;
            }
            have_n = true;
            exact = (o == 'N');
            nchars = nv;
            break;
          }
          case 't': {
            std::string v = val();
            char *end = nullptr;
            double tv = std::strtod(v.c_str(), &end);
            if (v.empty() || (end && *end) || tv < 0) {
              std::fprintf(stderr, "%sread: %s: invalid timeout specification\n",
                           sh.err_prefix().c_str(), v.c_str());
              return 1;
            }
            have_t = true;
            timeout = tv;
            break;
          }
          case 'i': (void)val(); break;              // initial text: needs readline; accepted
          case 'e': edit = true; break;              // readline editing
          case 's': case 'E': break;                 // silent: accepted
          default:
            std::fprintf(stderr, "%sread: -%c: invalid option\n", sh.err_prefix().c_str(), o);
            std::fprintf(stderr, "read: usage: read [-Eers] [-a array] [-d delim] [-i text] "
                                 "[-n nchars] [-N nchars] [-p prompt] [-t timeout] [-u fd] "
                                 "[name ...]\n");
            return 2;
        }
      }
    } else {
      names.push_back(a);
    }
  }

  // Validate the -u descriptor and the target names before reading, as bash
  // does, so a bad fd or an invalid identifier is diagnosed even on EOF.
  if (have_u && fcntl(fd, F_GETFD) == -1) {
    std::fprintf(stderr, "%sread: %d: invalid file descriptor: %s\n", sh.err_prefix().c_str(),
                 fd, std::strerror(errno));
    return 1;
  }
  auto valid_read_name = [](const std::string &s) {
    size_t b = s.find('[');
    std::string nm = b == std::string::npos ? s : s.substr(0, b);
    if (nm.empty() || !(std::isalpha(static_cast<unsigned char>(nm[0])) || nm[0] == '_'))
      return false;
    for (char c : nm)
      if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_')) return false;
    return true;
  };
  if (!arrayname.empty() && !valid_read_name(arrayname)) {
    std::fprintf(stderr, "%sread: `%s': not a valid identifier\n", sh.err_prefix().c_str(),
                 arrayname.c_str());
    return 1;
  }
  for (const std::string &nm : names)
    if (!valid_read_name(nm)) {
      std::fprintf(stderr, "%sread: `%s': not a valid identifier\n", sh.err_prefix().c_str(),
                   nm.c_str());
      return 1;
    }

  // $TMOUT is the default timeout, but only when reading from a terminal.
  if (!have_t && isatty(fd)) {
    std::string tm = sh.get("TMOUT");
    if (!tm.empty()) {
      char *end = nullptr;
      double tv = std::strtod(tm.c_str(), &end);
      if (end && *end == '\0' && tv > 0) { have_t = true; timeout = tv; }
    }
  }

  // With -e bash reads through readline, whose setup dominates a
  // sub-millisecond timeout: the read times out before seeing any input.
  if (edit && have_t && timeout > 0.0 && timeout < 0.001) {
    for (const std::string &nm : names) sh.set(nm, std::string());
    return 128 + SIGALRM;
  }

  // -t 0: don't read anything, just report whether input is available.
  if (have_t && timeout == 0.0) {
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(fd, &rfds);
    struct timeval tv = {0, 0};
    return select(fd + 1, &rfds, nullptr, nullptr, &tv) > 0 ? 0 : 1;
  }

  if (!prompt.empty() && isatty(fd)) { std::fputs(prompt.c_str(), stderr); std::fflush(stderr); }

  // Deadline for -t: reads must complete before it, or the read times out with
  // status 128+SIGALRM and whatever was read so far is still assigned.
  struct timeval start_tv;
  gettimeofday(&start_tv, nullptr);
  double deadline = have_t
      ? start_tv.tv_sec + start_tv.tv_usec / 1e6 + timeout
      : 0.0;

  bool eof = false, timed_out = false;
  // Read a byte, honoring the deadline: 1 = got byte, 0 = EOF/error, -1 = timeout.
  auto read_byte = [&](char &ch) -> int {
    if (have_t) {
      struct timeval now;
      gettimeofday(&now, nullptr);
      double remain = deadline - (now.tv_sec + now.tv_usec / 1e6);
      if (remain <= 0) return -1;
      fd_set rfds;
      FD_ZERO(&rfds);
      FD_SET(fd, &rfds);
      struct timeval tv;
      tv.tv_sec = static_cast<time_t>(remain);
      tv.tv_usec = static_cast<suseconds_t>((remain - static_cast<double>(tv.tv_sec)) * 1e6);
      if (select(fd + 1, &rfds, nullptr, nullptr, &tv) <= 0) return -1;
    }
    return ::read(fd, &ch, 1) == 1 ? 1 : 0;
  };

  // Read up to the delimiter (or `nchars' with -n/-N) byte by byte so we don't
  // consume input past it.  QUOTED marks backslash-escaped characters: they
  // are not IFS delimiters and are not stripped as trailing whitespace.
  std::string line;
  std::string quoted;
  if (!(have_n && nchars == 0)) {
    for (;;) {
      char ch;
      int r = read_byte(ch);
      if (r <= 0) { eof = (r == 0); timed_out = (r < 0); break; }
      if (ch == '\0' && delim != 0) continue;    // stray NULs are discarded
      if (!raw && ch == '\\') {                  // backslash escaping (unless -r)
        char nx;
        int r2 = read_byte(nx);
        if (r2 <= 0) { eof = (r2 == 0); timed_out = (r2 < 0); break; }  // trailing \ dropped
        if (nx == '\n') continue;                // line continuation: drop both
        line += nx;
        quoted += '\1';
        if (have_n && static_cast<long>(line.size()) >= nchars) break;
        continue;
      }
      if (!exact && static_cast<unsigned char>(ch) == static_cast<unsigned char>(delim)) break;
      line += ch;
      quoted += '\0';
      if (have_n && static_cast<long>(line.size()) >= nchars) break;
    }
  }

  const std::string ifs = sh.ifs();
  size_t p = 0;
  const size_t n = line.size();
  auto is_ifs = [&](size_t idx) {
    return quoted[idx] == '\0' && ifs.find(line[idx]) != std::string::npos;
  };
  auto is_ifs_ws = [&](size_t idx) {
    return is_ifs(idx) && std::isspace(static_cast<unsigned char>(line[idx]));
  };
  auto skip_ifs_ws = [&]() { while (p < n && is_ifs_ws(p)) p++; };
  // One field, then its trailing whitespace and at most one non-whitespace
  // delimiter (plus more whitespace) -- bash's get_word_from_string.
  auto get_word = [&]() {
    std::string w;
    while (p < n && !is_ifs(p)) w += line[p++];
    while (p < n && is_ifs_ws(p)) p++;
    if (p < n && is_ifs(p) && !is_ifs_ws(p)) {
      p++;
      while (p < n && is_ifs_ws(p)) p++;
    }
    return w;
  };

  int retval = timed_out ? 128 + SIGALRM : (eof ? 1 : 0);

  if (!arrayname.empty()) {                    // -a: all words into the array
    std::vector<std::pair<std::optional<std::string>, std::string>> elems;
    skip_ifs_ws();
    while (p < n) elems.emplace_back(std::nullopt, get_word());
    sh.array_assign(arrayname, elems, false, false);
    return retval;
  }
  if (names.empty()) {
    if (!sh.set("REPLY", line)) return 2;
    return retval;
  }
  if (exact) {
    // -N: assigned without word splitting.
    if (!sh.set(names[0], line)) return 2;
    for (size_t k = 1; k < names.size(); k++)
      if (!sh.set(names[k], std::string())) return 2;
    return retval;
  }

  // Assign fields to names; the last name gets the remainder: a lone final
  // field is assigned with its delimiters consumed, anything more keeps the
  // raw text with only trailing IFS whitespace stripped (as bash does).
  std::vector<std::string> fields;
  skip_ifs_ws();
  for (size_t k = 0; k + 1 < names.size(); k++)
    fields.push_back(p < n ? get_word() : std::string());
  std::string rest;
  if (p < n) {
    size_t save = p;
    std::string w = get_word();
    if (p >= n) {
      rest = w;
    } else {
      // Trailing IFS whitespace is stripped even when backslash-escaped: bash
      // strips the character and the later dequoting drops the lone escape.
      size_t e = n;
      while (e > save && std::isspace(static_cast<unsigned char>(line[e - 1])) &&
             ifs.find(line[e - 1]) != std::string::npos)
        e--;
      rest = line.substr(save, e - save);
    }
  }
  fields.push_back(rest);
  for (size_t k = 0; k < names.size(); k++) {
    std::string nm = names[k];
    if (!array_expand_once_name(sh, nm)) return 1;
    if (!sh.set(nm, k < fields.size() ? fields[k] : std::string())) return 2;
  }
  return retval;
}

std::string find_in_path(Shell &sh, const std::string &name) {
  if (name.find('/') != std::string::npos)
    return access(name.c_str(), X_OK) == 0 ? name : std::string();
  std::string path = sh.get("PATH");
  size_t i = 0;
  while (i <= path.size()) {
    size_t j = path.find(':', i);
    std::string dir = path.substr(i, j == std::string::npos ? std::string::npos : j - i);
    if (dir.empty()) dir = ".";
    std::string full = dir + "/" + name;
    if (access(full.c_str(), X_OK) == 0) return full;
    if (j == std::string::npos) break;
    i = j + 1;
  }
  return std::string();
}

static const char *const kBuiltinNames[] = {
    ":", "true", "false", "echo", "printf", "pwd", "cd", "export", "unset", "set",
    "read", "test", "[", "shift", "exit", "return", "break", "continue", "eval",
    "source", ".", "local", "declare", "typeset", "readonly", "let", "type", "trap",
    "umask", "getopts", "exec", "command", "times", "wait", "jobs", "fg", "bg",
    "disown", "kill", "suspend", "dirs", "pushd", "popd", "mapfile", "readarray",
    "help", "builtin", "logout", "hash", "shopt", "ulimit", "enable", "caller", "alias", "unalias", "history", "fc", "compgen", "complete", "compopt", "bind", "personality", nullptr};

bool is_builtin_name(const std::string &n) {
  for (int i = 0; kBuiltinNames[i]; i++)
    if (n == kBuiltinNames[i]) return true;
  return false;
}

std::vector<std::string> builtin_names_sorted() {
  std::vector<std::string> v;
  for (int i = 0; kBuiltinNames[i]; i++) v.emplace_back(kBuiltinNames[i]);
  std::sort(v.begin(), v.end());
  return v;
}

// Shared logic for declare/local/readonly/typeset.
int bi_declare(Shell &sh, const std::vector<std::string> &argv, bool force_local, bool force_ro) {
  bool mk_array = false, mk_assoc = false, integer = false, readonly = force_ro;
  bool exported = false, global = false, local = force_local, nameref = false;
  bool lcase = false, ucase = false;  // -l lowercase / -u uppercase attribute
  bool capcase = false;               // -c capitalize-first attribute
  bool funcs = false, funcnames = false;  // -f (definitions) / -F (names)
  bool fp = false;                        // -p (display reproducibly)
  size_t i = 1;
  // `+X' flags remove an attribute rather than adding it (`typeset +n foo').
  bool rm_integer = false, rm_readonly = false, rm_exported = false;
  bool rm_nameref = false, rm_lcase = false, rm_ucase = false, rm_capcase = false;
  for (; i < argv.size(); i++) {
    const std::string &a = argv[i];
    if (a == "--") { i++; break; }  // end-of-options marker
    if (a.size() >= 2 && (a[0] == '-' || a[0] == '+')) {
      bool add = a[0] == '-';
      // Valid option letters differ by command; `readonly' takes a far smaller
      // set than `declare'.  An unrecognized letter is an error with usage.
      static const char *kRoOpts = "aAfnp";
      static const char *kDeclOpts = "aAcfFgiIlnprtux";
      const char *allowed = force_ro ? kRoOpts : kDeclOpts;
      for (size_t k = 1; k < a.size(); k++) {
        if (!std::strchr(allowed, a[k])) {
          std::fprintf(stderr, "%s%s: %c%c: invalid option\n", sh.err_prefix().c_str(),
                       argv[0].c_str(), a[0], a[k]);
          if (force_ro)
            std::fprintf(stderr, "readonly: usage: readonly [-aAf] [name[=value] ...] "
                                 "or readonly -p\n");
          else if (argv[0] == "typeset")
            std::fprintf(stderr, "typeset: usage: typeset [-aAfFgiIlnrtux] name[=value] ... "
                                 "or typeset -p [-aAfFilnrtux] [name ...]\n");
          else if (argv[0] == "local")
            std::fprintf(stderr, "local: usage: local [option] name[=value] ...\n");
          else
            std::fprintf(stderr, "declare: usage: declare [-aAfFgiIlnrtux] [name[=value] ...] "
                                 "or declare -p [-aAfFilnrtux] [name ...]\n");
          return 2;
        }
        switch (a[k]) {
          case 'f': funcs = true; break;
          case 'F': funcnames = true; break;
          case 'a': mk_array = true; break;
          case 'A': mk_assoc = true; break;
          case 'i': (add ? integer : rm_integer) = true; break;
          case 'r': (add ? readonly : rm_readonly) = true; break;
          case 'x': (add ? exported : rm_exported) = true; break;
          case 'g': global = true; break;
          case 'n': (add ? nameref : rm_nameref) = true; break;
          case 'l': (add ? lcase : rm_lcase) = true; break;
          case 'u': (add ? ucase : rm_ucase) = true; break;
          case 'c': (add ? capcase : rm_capcase) = true; break;
          case 'p': fp = true; break;
          default: break;
        }
      }
    } else {
      break;
    }
  }
  // `-f'/`-F' cannot combine with the array/assoc/integer/nameref attributes;
  // bash names the offending option and fails (no usage line), preferring
  // -n over -i over -A over -a.
  if ((funcs || funcnames) && (mk_array || mk_assoc || integer || nameref)) {
    const char *opt = nameref ? "-n" : integer ? "-i" : mk_assoc ? "-A" : "-a";
    std::fprintf(stderr, "%s%s: %s: invalid option\n", sh.err_prefix().c_str(),
                 argv[0].c_str(), opt);
    return 1;
  }
  // -p: display variables (named ones, or all) in reproducible form.
  if (fp && !funcs && !funcnames) {
    int st = 0;
    if (i >= argv.size()) {
      if (force_local) {
        // `local -p': only the variables local to the current function scope,
        // in declaration order (bash), not every shell variable.
        if (!sh.local_stack.empty()) {
          std::vector<std::string> seen;
          for (const auto &pr : sh.local_stack.back()) {
            if (std::find(seen.begin(), seen.end(), pr.first) != seen.end()) continue;
            seen.push_back(pr.first);
            auto it = sh.vars.find(pr.first);
            if (it != sh.vars.end()) declare_print_var(pr.first, it->second);
          }
        }
      } else {
        // `declare -p' with attribute flags (`-pa', `-pi', `-pr', ...) lists
        // only the variables carrying those attributes, not every variable.
        for (const auto &kv : sh.vars) {
          const std::string &nm = kv.first;
          if (nm.empty() || !(std::isalpha(static_cast<unsigned char>(nm[0])) || nm[0] == '_'))
            continue;  // skip special parameters like $, ?, # (not real variables)
          const Variable &v = kv.second;
          if (mk_array && v.kind != VarKind::Indexed) continue;
          if (mk_assoc && v.kind != VarKind::Assoc) continue;
          if (nameref && !v.nameref) continue;
          if (readonly && !v.readonly) continue;
          if (integer && !v.integer) continue;
          if (exported && !v.exported) continue;
          if (lcase && !v.lcase) continue;
          if (ucase && !v.ucase) continue;
          if (capcase && !v.capcase) continue;
          declare_print_var(kv.first, v);
        }
      }
    } else {
      for (; i < argv.size(); i++) {
        auto it = sh.vars.find(argv[i]);
        if (it == sh.vars.end()) {
          std::fprintf(stderr, "%s%s: %s: not found\n", sh.err_prefix().c_str(),
                       argv[0].c_str(), argv[i].c_str());
          st = 1;
        } else {
          declare_print_var(argv[i], it->second);
        }
      }
    }
    return st;
  }
  // -f / -F: display function definitions or names.  With -x, restrict to
  // exported functions (gnash doesn't export functions, so none qualify).
  if (funcs || funcnames) {
    int st = 0;
    if (exported) return i >= argv.size() ? 0 : 1;
    if (i >= argv.size()) {  // all functions
      for (const auto &kv : sh.functions) {
        if (funcnames) std::printf("declare -f %s\n", kv.first.c_str());
        else std::printf("%s\n", named_function_string(kv.first, kv.second, sh.opt_posix).c_str());
      }
      return 0;
    }
    for (; i < argv.size(); i++) {
      auto it = sh.functions.find(argv[i]);
      if (it == sh.functions.end()) {
        // `declare -fp NAME' (the -p form) reports a missing function; the bare
        // `declare -f NAME' form fails silently.
        if (fp)
          std::fprintf(stderr, "%s%s: %s: not found\n", sh.err_prefix().c_str(),
                       argv[0].c_str(), argv[i].c_str());
        st = 1;
        continue;
      }
      if (funcnames) std::printf("%s\n", argv[i].c_str());
      else std::printf("%s\n", named_function_string(argv[i], it->second, sh.opt_posix).c_str());
    }
    return st;
  }

  // Attribute flags with no name arguments list every variable that carries all
  // of the requested attributes, in `declare -p' form: `typeset -n' lists all
  // namerefs, `declare -i' all integers, and so on.
  if (i >= argv.size() &&
      (nameref || readonly || integer || exported || mk_array || mk_assoc ||
       lcase || ucase || capcase)) {
    for (const auto &kv : sh.vars) {
      const std::string &nm = kv.first;
      if (nm.empty() || !(std::isalpha(static_cast<unsigned char>(nm[0])) || nm[0] == '_'))
        continue;  // skip special parameters like $, ?, # (not real variables)
      const Variable &v = kv.second;
      if (nameref && !v.nameref) continue;
      if (readonly && !v.readonly) continue;
      if (integer && !v.integer) continue;
      if (exported && !v.exported) continue;
      if (mk_array && v.kind != VarKind::Indexed) continue;
      if (mk_assoc && v.kind != VarKind::Assoc) continue;
      if (lcase && !v.lcase) continue;
      if (ucase && !v.ucase) continue;
      if (capcase && !v.capcase) continue;
      declare_print_var(kv.first, v);
    }
    return 0;
  }

  int ret = 0;
  for (; i < argv.size(); i++) {
    const std::string &a = argv[i];
    size_t nend = a.find_first_of("[=");
    std::string name = (nend == std::string::npos) ? a : a.substr(0, nend);
    size_t eq = a.find('=');
    // A `+=' just before the `=' means append.  For a bare scalar (`name+=')
    // the `+' is part of neither the name nor the value.
    bool append = eq != std::string::npos && eq > 0 && a[eq - 1] == '+';
    if (append && nend == eq) name = a.substr(0, eq - 1);
    // For a plain scalar assignment, expand the RHS in the ENCLOSING scope
    // before the variable is localized, so `local x=${x-10}' / `local x=$x'
    // reference the outer (or temporary-environment) value, as bash does.
    bool arraylit0 = eq != std::string::npos && a.size() > eq + 2 &&
                     a[eq + 1] == '(' && a.back() == ')';
    bool subscript0 = nend != std::string::npos && a[nend] == '[';
    // `readonly'/`export' cannot target a single array element (`readonly a[5]')
    // -- bash reports it as an invalid identifier; declare/typeset/local can.
    // (zsh's array/readonly rules differ, so leave that personality alone.)
    if (subscript0 && !sh.is_zsh() && (argv[0] == "readonly" || argv[0] == "export")) {
      std::string tgt = (eq == std::string::npos) ? a : a.substr(0, eq);
      std::fprintf(stderr, "%s%s: `%s': not a valid identifier\n", sh.err_prefix().c_str(),
                   argv[0].c_str(), tgt.c_str());
      ret = 1;
      continue;
    }
    // A nameref cannot itself be an array element (`declare -n a[128]'): bash
    // rejects the subscripted name outright.  (A subscript in the *value*,
    // `declare -n r=a[2]', has `=' before `[' so subscript0 is false.)
    if (subscript0 && nameref) {
      std::string tgt = (eq == std::string::npos) ? a : a.substr(0, eq);
      std::fprintf(stderr, "%s%s: %s: reference variable cannot be an array\n",
                   sh.err_prefix().c_str(), argv[0].c_str(), tgt.c_str());
      ret = 1;
      continue;
    }
    bool scalar_pre = eq != std::string::npos && !arraylit0 && !subscript0 && !nameref;
    std::string pre_val;
    if (scalar_pre) { Expander ex(sh); pre_val = ex.expand_assignment(a.substr(eq + 1)); }
    // `declare -g name' assigns to the GLOBAL binding even when a local of the
    // same name shadows it.  Temporarily swap the saved global binding into
    // `vars', run the normal assignment against it, and restore the local at
    // the end of the loop body.  (The RHS was already expanded above against the
    // still-visible local, matching bash.)
    std::optional<Variable> *gslot = nullptr;
    Variable held_local;
    bool had_local = false;
    if (global) {
      for (auto &scope : sh.local_stack) {
        for (auto &e : scope)
          if (e.first == name) { gslot = &e.second; break; }
        if (gslot) break;
      }
      if (gslot) {
        auto vit = sh.vars.find(name);
        if ((had_local = (vit != sh.vars.end()))) held_local = vit->second;
        if (gslot->has_value()) sh.vars[name] = gslot->value();
        else sh.vars.erase(name);
      }
    }
    if (local && !global) sh.make_local(name);
    // An array cannot be switched between indexed and associative in place; bash
    // rejects the redeclaration and leaves the variable unchanged.
    if (mk_array || mk_assoc) {
      auto av = sh.vars.find(name);
      if (av != sh.vars.end()) {
        if (mk_assoc && av->second.kind == VarKind::Indexed) {
          std::fprintf(stderr, "%s%s: %s: cannot convert indexed to associative array\n",
                       sh.err_prefix().c_str(), argv[0].c_str(), name.c_str());
          ret = 1;
          continue;
        }
        if (mk_array && av->second.kind == VarKind::Assoc) {
          std::fprintf(stderr, "%s%s: %s: cannot convert associative to indexed array\n",
                       sh.err_prefix().c_str(), argv[0].c_str(), name.c_str());
          ret = 1;
          continue;
        }
      }
    }
    if (mk_assoc) sh.make_array(name, true);
    else if (mk_array) sh.make_array(name, false);
    // A subscripted name implies an indexed array even without `-a'
    // (`declare -r c[100]' creates an empty readonly array c).
    else if (subscript0) sh.make_array(name, false);
    if (eq != std::string::npos) {
      std::string val = a.substr(eq + 1);
      bool arraylit = val.size() >= 2 && val.front() == '(' && val.back() == ')';
      bool subscript = nend != std::string::npos && a[nend] == '[';
      // A quoted compound value (`declare -a d='(...)'`) is an array literal
      // when the target is an array: unwrap one layer of matched outer quotes so
      // the parentheses are recognized and the elements expanded.  Any subscript
      // is dropped -- the compound replaces the whole array, as in bash.
      auto cur = sh.vars.find(name);
      bool arrayvar = cur != sh.vars.end() &&
          (cur->second.kind == VarKind::Indexed || cur->second.kind == VarKind::Assoc);
      if (!arraylit && arrayvar && val.size() >= 4 &&
          (val.front() == '\'' || val.front() == '"') && val.back() == val.front() &&
          val[1] == '(' && val[val.size() - 2] == ')') {
        apply_assignment_word(sh, name + (append ? "+=" : "=") +
                                      val.substr(1, val.size() - 2));
      } else if (arraylit || subscript) {
        apply_assignment_word(sh, a);  // NAME=(...) or NAME[i]=...
      } else if (nameref) {
        // `declare -n ref=target': store the target NAME as ref's own value.
        // Bypass Shell::set's deref so retargeting an existing nameref rewrites
        // the reference itself rather than writing through to its old target.
        Expander ex(sh);
        val = ex.expand_assignment(val);
        bool preexist = sh.vars.count(name) != 0;
        auto pv = sh.vars.find(name);
        std::string tgt =
            (append && pv != sh.vars.end()) ? pv->second.value + val : val;
        // A nameref may not point at itself (`declare -n r=r'), either directly
        // or via one of its own elements (`r=r[0]').  At function scope a
        // same-name target instead refers to the enclosing variable, so bash
        // warns of a circular reference rather than erroring.
        bool selfref = (tgt == name);
        if (!selfref && !tgt.empty()) {
          size_t lb = tgt.find('[');
          if (lb != std::string::npos && tgt.back() == ']' &&
              tgt.substr(0, lb) == name)
            selfref = true;
        }
        if (selfref && !sh.in_function()) {
          // A direct `=' self reference is rejected by the declare builtin, so
          // the message carries the builtin name.  One that only becomes a self
          // reference after `+=' concatenation slips past that check and is
          // caught later while binding the value, which prints it bare.
          if (append)
            std::fprintf(stderr,
                         "%s%s: nameref variable self references not allowed\n",
                         sh.err_prefix().c_str(), name.c_str());
          else
            std::fprintf(stderr,
                         "%s%s: %s: nameref variable self references not allowed\n",
                         sh.err_prefix().c_str(), argv[0].c_str(), name.c_str());
          ret = 1;
          if (!preexist) sh.vars.erase(name);
          continue;
        }
        if (selfref) {
          // Warn once with the builtin prefix and once without, matching bash's
          // declare.def (builtin_warning) plus bind_variable_value.
          std::fprintf(stderr, "%s%s: warning: %s: circular name reference\n",
                       sh.err_prefix().c_str(), argv[0].c_str(), name.c_str());
          std::fprintf(stderr, "%swarning: %s: circular name reference\n",
                       sh.err_prefix().c_str(), name.c_str());
        }
        // An explicit empty target (`declare -n r=') is rejected as an invalid
        // identifier and does not create the nameref (bash reports the empty
        // name); at global scope no variable survives, in a function the local
        // that was just made stays as a plain variable.
        if (tgt.empty()) {
          std::fprintf(stderr, "%s%s: `': not a valid identifier\n",
                       sh.err_prefix().c_str(), argv[0].c_str());
          ret = 1;
          if (!preexist) sh.vars.erase(name);
          continue;
        }
        // The target must be a valid nameref target (`foo' or `foo[2]').  bash
        // rejects anything else and does not create the variable.
        if (!tgt.empty() && !Shell::valid_nameref_target(tgt)) {
          std::fprintf(stderr,
                       "%s%s: `%s': invalid variable name for name reference\n",
                       sh.err_prefix().c_str(), argv[0].c_str(), tgt.c_str());
          ret = 1;
          if (!preexist) sh.vars.erase(name);  // no half-created variable
          continue;
        }
        Variable &rv = sh.vars[name];
        if (rv.readonly) {
          std::fprintf(stderr, "%s%s: readonly variable\n", sh.err_prefix().c_str(),
                       name.c_str());
          return 1;
        }
        rv.value = tgt;
      } else {
        val = pre_val;  // expanded above, in the enclosing scope, before localizing
        // Honor a pre-existing integer attribute too (e.g. `readonly x+=7' on a
        // variable already declared `-i'), not just an `-i' on this command.
        auto exv = sh.vars.find(name);
        bool eff_integer = integer || (exv != sh.vars.end() && exv->second.integer);
        if (eff_integer) {
          bool ok = true;
          long long rhs = eval_arith(sh, val, &ok);
          long long base = append ? eval_arith(sh, sh.get(name), &ok) : 0;
          val = std::to_string(base + rhs);
        } else if (append) {
          val = sh.get(name) + val;
        }
        if (lcase) for (char &c : val) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        else if (ucase) for (char &c : val) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        else if (capcase) {
          bool cfirst = true;
          for (char &c : val) {
            c = static_cast<char>(cfirst ? std::toupper(static_cast<unsigned char>(c))
                                         : std::tolower(static_cast<unsigned char>(c)));
            cfirst = false;
          }
        }
        // Retargeting an existing empty nameref via `declare r=val' validates
        // the value like a plain assignment, but the diagnostic carries the
        // builtin name (Shell::set would print it bare).
        auto nrit = sh.vars.find(name);
        if (nrit != sh.vars.end() && nrit->second.nameref &&
            nrit->second.value.empty() && !val.empty() &&
            !Shell::valid_nameref_target(val)) {
          // At function scope bash validates the value as a nameref target
          // ("invalid variable name for name reference"); at global scope it
          // reports it as a plain invalid identifier.
          if (sh.in_function())
            std::fprintf(stderr,
                         "%s%s: `%s': invalid variable name for name reference\n",
                         sh.err_prefix().c_str(), argv[0].c_str(), val.c_str());
          else
            std::fprintf(stderr, "%s%s: `%s': not a valid identifier\n",
                         sh.err_prefix().c_str(), argv[0].c_str(), val.c_str());
          ret = 1;
          // At global scope the still-invisible nameref is discarded (a later
          // `declare -p' reports it as not found); a function-local one made by
          // this scope persists.
          if (!sh.in_function()) sh.vars.erase(name);
          continue;
        }
        sh.set(name, val);
      }
    }
    // Applying an attribute to an existing nameref (without a `-n'/`+n' on this
    // command) follows the reference and applies it to the target, creating the
    // target if needed -- e.g. `readonly ref' marks ref's target readonly.  bash
    // resolves the nameref because -n is not present.
    std::string aname = name;
    if (!nameref && !rm_nameref) {
      auto nit = sh.vars.find(name);
      if (nit != sh.vars.end() && nit->second.nameref) aname = sh.deref(name);
    }
    Variable &v = sh.vars[aname];
    if (readonly) v.readonly = true;
    if (exported) v.exported = true;
    if (integer) v.integer = true;
    if (ucase) v.ucase = true;
    if (lcase) v.lcase = true;
    if (capcase) v.capcase = true;
    // Mark the nameref last, so the assignment above stored the *target name*
    // as this variable's value rather than being redirected through it.  Adding
    // `-n' to a variable whose existing value is not a valid nameref target
    // (`r=/; declare -n r') -- or which names itself -- is rejected; bash leaves
    // the variable unchanged without the attribute.  (The `=val' form validated
    // its value above.)
    if (nameref && (v.kind == VarKind::Indexed || v.kind == VarKind::Assoc)) {
      // An existing array/associative variable cannot be turned into a nameref;
      // bash rejects the `-n' and leaves the array (and any assignment made by
      // this same command) intact.
      std::fprintf(stderr,
                   "%s%s: %s: reference variable cannot be an array\n",
                   sh.err_prefix().c_str(), argv[0].c_str(), name.c_str());
      ret = 1;
    } else if (nameref && v.value == name && !sh.in_function()) {
      std::fprintf(stderr,
                   "%s%s: %s: nameref variable self references not allowed\n",
                   sh.err_prefix().c_str(), argv[0].c_str(), name.c_str());
      ret = 1;
    } else if (nameref && !v.value.empty() && v.value != name &&
               !Shell::valid_nameref_target(v.value)) {
      std::fprintf(stderr,
                   "%s%s: `%s': invalid variable name for name reference\n",
                   sh.err_prefix().c_str(), argv[0].c_str(), v.value.c_str());
      ret = 1;
    } else if (nameref) {
      v.nameref = true;
    }
    // `+X' removes attributes.  Applied after the assignment so `typeset +n
    // foo=other' writes through the still-active nameref to its target before
    // the reference is torn down, matching bash.
    if (rm_readonly) v.readonly = false;
    if (rm_exported) v.exported = false;
    if (rm_integer) v.integer = false;
    if (rm_nameref) v.nameref = false;
    if (rm_ucase) v.ucase = false;
    if (rm_lcase) v.lcase = false;
    if (rm_capcase) v.capcase = false;
    // Restore the local shadow after a `declare -g' assignment to the global.
    if (gslot) {
      auto vit = sh.vars.find(name);
      *gslot = (vit != sh.vars.end()) ? std::optional<Variable>(vit->second) : std::nullopt;
      if (had_local) sh.vars[name] = held_local;
      else sh.vars.erase(name);
    }
  }
  return ret;
}

int bi_let(Shell &sh, const std::vector<std::string> &argv) {
  if (argv.size() < 2) {  // `let' with no expression
    std::fprintf(stderr, "%slet: expression expected\n", sh.err_prefix().c_str());
    return 1;
  }
  long long last = 0;
  bool ok = true;
  for (size_t i = 1; i < argv.size(); i++) last = eval_arith_msg(sh, argv[i], "let", &ok);
  return (ok && last != 0) ? 0 : 1;
}

// A bash reserved word (shell keyword).
bool is_reserved_word(const std::string &w) {
  static const char *kw[] = {"!",  "{",     "}",    "[[",   "]]",   "time",  "case",
                             "coproc", "do",  "done", "elif", "else", "esac", "fi",
                             "for", "function", "if", "in",   "select", "then",
                             "until", "while", nullptr};
  for (int i = 0; kw[i]; i++)
    if (w == kw[i]) return true;
  return false;
}

// Every executable named `name' along $PATH, in search order.
std::vector<std::string> find_all_in_path(Shell &sh, const std::string &name) {
  std::vector<std::string> out;
  if (name.find('/') != std::string::npos) {
    if (access(name.c_str(), X_OK) == 0) out.push_back(name);
    return out;
  }
  std::string path = sh.get("PATH");
  size_t i = 0;
  while (i <= path.size()) {
    size_t j = path.find(':', i);
    std::string dir = path.substr(i, j == std::string::npos ? std::string::npos : j - i);
    if (dir.empty()) dir = ".";
    std::string full = dir + "/" + name;
    if (access(full.c_str(), X_OK) == 0) out.push_back(full);
    if (j == std::string::npos) break;
    i = j + 1;
  }
  return out;
}

// type [-tpPaf] name...
//   -t  print one word: alias/keyword/function/builtin/file
//   -p  print the disk file that would run (empty if shadowed by a non-file)
//   -P  force a $PATH search, ignoring functions/builtins/keywords
//   -a  report every location, not just the first
//   -f  suppress shell-function lookup
int bi_type(Shell &sh, const std::vector<std::string> &argv) {
  bool ft = false, fp = false, fP = false, fa = false, ff = false;
  size_t i = 1;
  for (; i < argv.size(); i++) {
    const std::string &a = argv[i];
    if (a == "--") { i++; break; }
    if (a.size() < 2 || a[0] != '-') break;
    bool ok = true;
    for (size_t k = 1; k < a.size() && ok; k++) {
      switch (a[k]) {
        case 't': ft = true; break;
        case 'p': fp = true; break;
        case 'P': fP = true; break;
        case 'a': fa = true; break;
        case 'f': ff = true; break;
        default:
          std::fprintf(stderr, "%stype: -%c: invalid option\n", sh.err_prefix().c_str(), a[k]);
          std::fprintf(stderr, "type: usage: type [-afptP] name [name ...]\n");
          return 2;
      }
    }
    if (!ok) break;
  }

  int st = 0;
  for (; i < argv.size(); i++) {
    const std::string &n = argv[i];

    // -P forces a PATH search and ignores everything else.
    if (fP) {
      auto files = find_all_in_path(sh, n);
      if (files.empty()) { st = 1; continue; }
      for (size_t k = 0; k < (fa ? files.size() : 1u); k++)
        std::printf("%s\n", ft ? "file" : files[k].c_str());
      continue;
    }

    // Ordered candidate locations: keyword, function, builtin, then files.
    struct Loc { char kind; std::string text; };  // a/k/f/b/F
    std::vector<Loc> locs;
    if (sh.aliases.count(n)) locs.push_back({'a', sh.aliases.at(n)});
    if (is_reserved_word(n)) locs.push_back({'k', {}});
    if (!ff && sh.functions.count(n)) locs.push_back({'f', {}});
    if (is_builtin_name(n)) locs.push_back({'b', {}});
    for (const std::string &f : find_all_in_path(sh, n)) locs.push_back({'F', f});

    if (locs.empty()) {
      if (!ft && !fp) {
        std::fflush(stdout);  // keep interleaving with prior stdout lines
        std::fprintf(stderr, "%stype: %s: not found\n", sh.err_prefix().c_str(), n.c_str());
      }
      st = 1;
      continue;
    }

    size_t count = fa ? locs.size() : 1;
    for (size_t li = 0; li < count; li++) {
      const Loc &L = locs[li];
      if (ft) {
        std::printf("%s\n", L.kind == 'a' ? "alias"
                          : L.kind == 'k' ? "keyword"
                          : L.kind == 'f' ? "function"
                          : L.kind == 'b' ? "builtin"
                                          : "file");
      } else if (fp) {
        if (L.kind == 'F') std::printf("%s\n", L.text.c_str());
      } else {
        switch (L.kind) {
          case 'a':
            std::printf("%s is aliased to `%s'\n", n.c_str(), L.text.c_str());
            break;
          case 'k': std::printf("%s is a shell keyword\n", n.c_str()); break;
          case 'f':
            std::printf("%s is a function\n%s\n", n.c_str(),
                        named_function_string(n, sh.functions[n]).c_str());
            break;
          case 'b': std::printf("%s is a shell builtin\n", n.c_str()); break;
          case 'F': std::printf("%s is %s\n", n.c_str(), L.text.c_str()); break;
        }
      }
    }
  }
  return st;
}

int signame_to_num(const std::string &s);

// Canonical trap key for a signal number (must match shell.cpp's mapping).
const char *trapname_from_num(int sig) {
  switch (sig) {
    case SIGHUP: return "HUP";   case SIGINT: return "INT";
    case SIGQUIT: return "QUIT"; case SIGILL: return "ILL";
    case SIGTRAP: return "TRAP"; case SIGABRT: return "ABRT";
    case SIGFPE: return "FPE";   case SIGKILL: return "KILL";
    case SIGBUS: return "BUS";   case SIGSEGV: return "SEGV";
    case SIGSYS: return "SYS";   case SIGPIPE: return "PIPE";
    case SIGALRM: return "ALRM"; case SIGTERM: return "TERM";
    case SIGURG: return "URG";   case SIGSTOP: return "STOP";
    case SIGTSTP: return "TSTP"; case SIGCONT: return "CONT";
    case SIGCHLD: return "CHLD"; case SIGTTIN: return "TTIN";
    case SIGTTOU: return "TTOU"; case SIGXCPU: return "XCPU";
    case SIGXFSZ: return "XFSZ"; case SIGVTALRM: return "VTALRM";
    case SIGPROF: return "PROF"; case SIGWINCH: return "WINCH";
    case SIGUSR1: return "USR1"; case SIGUSR2: return "USR2";
#ifdef SIGEMT
    case SIGEMT: return "EMT";
#endif
#ifdef SIGIO
    case SIGIO: return "IO";
#endif
#ifdef SIGINFO
    case SIGINFO: return "INFO";
#endif
    default: return nullptr;
  }
}

// `kill -l' / `trap -l': list every named signal in bash's 5-column layout,
// tab-separated with the number right-justified to the widest signal number.
void print_signal_list() {
  std::vector<std::pair<int, const char *>> sigs;
  for (int s = 1; s < NSIG; s++)
    if (const char *nm = trapname_from_num(s)) sigs.emplace_back(s, nm);
  int width = sigs.empty() ? 1
                           : static_cast<int>(std::to_string(sigs.back().first).size());
  for (size_t k = 0; k < sigs.size(); k++) {
    std::printf("%*d) SIG%s", width, sigs[k].first, sigs[k].second);
    std::printf((k + 1) % 5 == 0 ? "\n" : "\t");
  }
  if (sigs.size() % 5 != 0) std::printf("\n");
}

// bash's trap -p sort order: EXIT (signal 0) first, then real signals by
// ascending signal number, then the pseudo-signals DEBUG, ERR, RETURN.
static int trap_order(const std::string &key) {
  if (key == "EXIT" || key == "0") return -1;
  if (key == "DEBUG") return 1000;
  if (key == "ERR") return 1001;
  if (key == "RETURN") return 1002;
  return signame_to_num(key);
}

// The name bash prints for a trap key: EXIT/DEBUG/ERR/RETURN bare, a real
// signal with the `SIG' prefix (`HUP' -> `SIGHUP').
static std::string trap_display_name(const std::string &key) {
  if (key == "0") return "EXIT";
  if (key == "EXIT" || key == "DEBUG" || key == "ERR" || key == "RETURN") return key;
  return "SIG" + key;
}

// The pseudo-signal name for a spec, or "" if it is a real OS signal.  Signal 0
// is the EXIT pseudo-signal.
static std::string trap_pseudo(const std::string &spec) {
  std::string u = spec;
  if (u.rfind("SIG", 0) == 0 || u.rfind("sig", 0) == 0) u = u.substr(3);
  for (char &c : u) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  if (u == "EXIT" || spec == "0") return "EXIT";
  if (u == "DEBUG" || u == "ERR" || u == "RETURN") return u;
  return "";
}

// The sh.traps key for a signal spec (pseudo name, or the canonical short name).
static std::string trap_key_for_spec(const std::string &spec) {
  std::string pseudo = trap_pseudo(spec);
  if (!pseudo.empty()) return pseudo;
  const char *canon = trapname_from_num(signame_to_num(spec));
  if (canon) return canon;
  std::string u = spec;
  if (u.rfind("SIG", 0) == 0 || u.rfind("sig", 0) == 0) u = u.substr(3);
  for (char &c : u) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return u;
}

// True if SPEC names a real signal: a number with a known name, or a signal
// name (with or without the `SIG' prefix).  signame_to_num() falls back to
// SIGTERM for garbage, so a name is confirmed by round-tripping to canonical.
static bool valid_signal_spec(const std::string &spec) {
  if (spec.empty()) return false;
  if (std::isdigit(static_cast<unsigned char>(spec[0]))) {
    char *end = nullptr;
    long n = std::strtol(spec.c_str(), &end, 10);
    return *end == '\0' && trapname_from_num(static_cast<int>(n)) != nullptr;
  }
  std::string n = spec;
  if (n.rfind("SIG", 0) == 0) n = n.substr(3);
  for (char &c : n) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  const char *canon = trapname_from_num(signame_to_num(spec));
  return canon && n == canon;
}

int bi_trap(Shell &sh, const std::vector<std::string> &argv) {
  static const char *kUsage = "trap: usage: trap [-Plp] [[action] signal_spec ...]";
  bool opt_p = false, opt_P = false, opt_l = false;
  size_t i = 1;
  for (; i < argv.size(); i++) {
    const std::string &a = argv[i];
    if (a == "--") { i++; break; }
    if (a.size() < 2 || a[0] != '-') break;  // action word or signal (`-' = reset)
    bool bad = false;
    for (size_t k = 1; k < a.size(); k++) {
      if (a[k] == 'p') opt_p = true;
      else if (a[k] == 'P') opt_P = true;
      else if (a[k] == 'l') opt_l = true;
      else {
        std::fprintf(stderr, "%strap: -%c: invalid option\n", sh.err_prefix().c_str(), a[k]);
        std::fprintf(stderr, "%s\n", kUsage);
        bad = true;
        break;
      }
    }
    if (bad) return 2;
  }

  if (opt_p && opt_P) {
    std::fprintf(stderr, "%strap: cannot specify both -p and -P\n", sh.err_prefix().c_str());
    return 1;
  }

  // -l: list the signal names (like `kill -l').
  if (opt_l) { print_signal_list(); return 0; }

  // -P: print just the action(s) of the named signal(s); at least one required.
  if (opt_P) {
    if (i >= argv.size()) {
      std::fprintf(stderr, "%strap: -P requires at least one signal name\n",
                   sh.err_prefix().c_str());
      return 1;
    }
    int st = 0;
    for (; i < argv.size(); i++) {
      if (trap_pseudo(argv[i]).empty() && !valid_signal_spec(argv[i])) {
        std::fprintf(stderr, "%strap: %s: invalid signal specification\n",
                     sh.err_prefix().c_str(), argv[i].c_str());
        st = 1;
        continue;
      }
      auto it = sh.traps.find(trap_key_for_spec(argv[i]));
      if (it != sh.traps.end()) std::printf("%s\n", it->second.c_str());
    }
    return st;
  }

  // Listing: a bare `trap', or `trap -p' with an optional signal filter.
  if (opt_p || i >= argv.size()) {
    std::vector<const std::pair<const std::string, std::string> *> items;
    int st = 0;
    if (i >= argv.size()) {
      for (const auto &kv : sh.traps) items.push_back(&kv);
    } else {
      for (; i < argv.size(); i++) {
        if (trap_pseudo(argv[i]).empty() && !valid_signal_spec(argv[i])) {
          std::fprintf(stderr, "%strap: %s: invalid signal specification\n",
                       sh.err_prefix().c_str(), argv[i].c_str());
          st = 1;
          continue;
        }
        auto it = sh.traps.find(trap_key_for_spec(argv[i]));
        if (it != sh.traps.end()) items.push_back(&*it);
      }
    }
    std::stable_sort(items.begin(), items.end(),
                     [](const auto *a, const auto *b) {
                       return trap_order(a->first) < trap_order(b->first);
                     });
    for (const auto *kv : items)
      std::printf("trap -- '%s' %s\n", kv->second.c_str(),
                  trap_display_name(kv->first).c_str());
    return st;
  }

  // Otherwise set/reset/ignore: the first operand is the action, the rest are
  // the signals it applies to.
  std::string cmd = argv[i];
  bool reset = (cmd == "-");
  bool ignore = cmd.empty();
  int st = 0;
  for (size_t j = i + 1; j < argv.size(); j++) {
    const std::string &spec = argv[j];
    std::string pseudo = trap_pseudo(spec);
    if (!pseudo.empty()) {  // EXIT/DEBUG/ERR/RETURN run at points, not on delivery
      if (reset) sh.traps.erase(pseudo);
      else sh.traps[pseudo] = cmd;
      continue;
    }
    // A spec that names no real signal is an error, and that signal is skipped.
    if (!valid_signal_spec(spec)) {
      std::fprintf(stderr, "%strap: %s: invalid signal specification\n",
                   sh.err_prefix().c_str(), spec.c_str());
      st = 1;
      continue;
    }
    int signo = signame_to_num(spec);
    std::string key = trap_key_for_spec(spec);
    if (reset) {
      sh.traps.erase(key);
      sh.set_signal_trap(signo, false);
    } else if (ignore) {
      sh.traps[key] = "";  // an ignored signal lists as `trap -- '' SIG'
      signal(signo, SIG_IGN);
    } else {
      sh.traps[key] = cmd;
      sh.set_signal_trap(signo, true);
    }
  }
  return st;
}

// Apply a chmod-style symbolic mode (`u=rwx,g-w,a+x') to ALLOWED (the file
// permission bits the umask permits).  Returns false on a malformed clause.
// Apply a chmod-style symbolic mode; on failure set errch to the offending
// character and erropr=true if it appeared where an operator (+-=) was expected
// (else it is an invalid mode character).  Mirrors bash parse_symbolic_mode.
static bool umask_symbolic(const std::string &s, mode_t &allowed, char &errch, bool &erropr) {
  size_t i = 0;
  while (i < s.size()) {
    int who = 0;
    while (i < s.size() && (s[i] == 'u' || s[i] == 'g' || s[i] == 'o' || s[i] == 'a')) {
      if (s[i] == 'u') who |= 0700;
      else if (s[i] == 'g') who |= 0070;
      else if (s[i] == 'o') who |= 0007;
      else who |= 0777;
      i++;
    }
    if (who == 0) who = 0777;  // no who: default to `a'
  start_op:
    {
      char op = (i < s.size()) ? s[i] : '\0';
      i++;
      if (op != '+' && op != '-' && op != '=') { errch = op; erropr = true; return false; }
      int perms = 0;
      while (i < s.size() && std::strchr("rwxXst", s[i])) {
        if (s[i] == 'r') perms |= 0444;
        else if (s[i] == 'w') perms |= 0222;
        else if (s[i] == 'x' || s[i] == 'X') perms |= 0111;
        // s/t (setuid/sticky) live outside the 0777 umask -- consume, ignore.
        i++;
      }
      int bits = who & perms;
      if (op == '=') allowed = (allowed & ~who) | bits;
      else if (op == '+') allowed |= bits;
      else allowed &= ~bits;
    }
    if (i >= s.size()) break;
    else if (s[i] == ',') i++;
    else if (s[i] == '+' || s[i] == '-' || s[i] == '=') goto start_op;
    else { errch = s[i]; erropr = false; return false; }
  }
  return true;
}

static std::string umask_to_symbolic(mode_t mask) {
  mode_t allowed = ~mask & 0777;
  std::string r;
  const char *who = "ugo";
  for (int k = 0; k < 3; k++) {
    int bits = (allowed >> (6 - 3 * k)) & 7;
    if (k) r += ',';
    r += who[k];
    r += '=';
    if (bits & 4) r += 'r';
    if (bits & 2) r += 'w';
    if (bits & 1) r += 'x';
  }
  return r;
}

int bi_umask(Shell &sh, const std::vector<std::string> &argv) {
  bool sym = false, print = false;
  size_t i = 1;
  for (; i < argv.size() && argv[i].size() >= 2 && argv[i][0] == '-'; i++) {
    for (size_t k = 1; k < argv[i].size(); k++) {
      if (argv[i][k] == 'S') sym = true;
      else if (argv[i][k] == 'p') print = true;
      else {
        std::fprintf(stderr, "%sumask: -%c: invalid option\n", sh.err_prefix().c_str(),
                     argv[i][k]);
        std::fprintf(stderr, "umask: usage: umask [-p] [-S] [mode]\n");
        return 2;
      }
    }
  }
  mode_t cur = umask(0);
  umask(cur);
  bool have_mode = i < argv.size();
  if (have_mode) {
    const std::string &mode = argv[i];
    if (!mode.empty() && std::isdigit(static_cast<unsigned char>(mode[0]))) {
      // A numeric mode must be all octal digits (bash reports a stray 8/9 as
      // out of range, but accepts values wider than 0777, e.g. setuid bits).
      if (mode.find_first_not_of("01234567") != std::string::npos) {
        std::fprintf(stderr, "%sumask: %s: octal number out of range\n",
                     sh.err_prefix().c_str(), mode.c_str());
        return 1;
      }
      cur = static_cast<mode_t>(std::strtol(mode.c_str(), nullptr, 8)) & 0777;
    } else {
      mode_t allowed = ~cur & 0777;
      char errch = 0;
      bool erropr = false;
      if (!umask_symbolic(mode, allowed, errch, erropr)) {
        std::fprintf(stderr, "%sumask: `%c': invalid symbolic mode %s\n",
                     sh.err_prefix().c_str(), errch, erropr ? "operator" : "character");
        return 1;
      }
      cur = ~allowed & 0777;
    }
    umask(cur);
  }
  // Print when no mode was given, or when -S/-p explicitly asks for output.
  if (!have_mode || sym || print) {
    char oct[8];
    std::snprintf(oct, sizeof(oct), "%04o", cur);
    std::string body = sym ? umask_to_symbolic(cur) : std::string(oct);
    if (print)
      std::printf("umask %s%s\n", sym ? "-S " : "", body.c_str());
    else
      std::printf("%s\n", body.c_str());
  }
  return 0;
}

int bi_getopts(Shell &sh, const std::vector<std::string> &argv) {
  static const char *kUsage = "getopts: usage: getopts optstring name [arg ...]";
  // Character-scan state lives on the shell (bash's sh_charindex/nextchar) so
  // it is saved and restored around a function's `local OPTIND'.
  int &s_optind = sh.getopt_optind;
  size_t &s_charidx = sh.getopt_charidx;
  std::string &s_curarg = sh.getopt_curarg;

  if (argv.size() >= 2 && argv[1].size() >= 2 && argv[1][0] == '-' && argv[1] != "--") {
    std::fprintf(stderr, "%sgetopts: -%c: invalid option\n", sh.err_prefix().c_str(),
                 argv[1][1]);
    std::fprintf(stderr, "%s\n", kUsage);
    return 2;
  }
  if (argv.size() < 3) {
    std::fprintf(stderr, "%s\n", kUsage);
    return 2;
  }
  std::string optstring = argv[1];
  const std::string &name = argv[2];
  {
    bool ok = !name.empty() &&
              (std::isalpha(static_cast<unsigned char>(name[0])) || name[0] == '_');
    for (char c : name)
      if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_')) ok = false;
    if (!ok) {
      std::fprintf(stderr, "%sgetopts: `%s': not a valid identifier\n",
                   sh.err_prefix().c_str(), name.c_str());
      // bash has already consumed the current option-argument at this point,
      // so advance OPTIND past a leading option so `shift $((OPTIND-1))' works.
      int oi = 1;
      { std::string v; if (sh.get_if_set("OPTIND", v)) oi = std::atoi(v.c_str()); }
      std::vector<std::string> a = argv.size() > 3
          ? std::vector<std::string>(argv.begin() + 3, argv.end())
          : sh.positional;
      if (oi - 1 >= 0 && oi - 1 < static_cast<int>(a.size()) &&
          !a[static_cast<size_t>(oi - 1)].empty() && a[static_cast<size_t>(oi - 1)][0] == '-' &&
          a[static_cast<size_t>(oi - 1)] != "--")
        sh.set("OPTIND", std::to_string(oi + 1));
      return 2;
    }
  }
  std::vector<std::string> args;
  if (argv.size() > 3) args.assign(argv.begin() + 3, argv.end());
  else args = sh.positional;

  bool silent = !optstring.empty() && optstring[0] == ':';
  if (silent) optstring.erase(0, 1);
  // The `opterr' shell variable (default 1) also enables silent mode when 0.
  { std::string v; if (sh.get_if_set("OPTERR", v) && v == "0") silent = true; }

  int optind = 1;
  { std::string v; if (sh.get_if_set("OPTIND", v)) optind = std::atoi(v.c_str()); }
  if (optind < 1) optind = 1;
  // Restart at the first character when OPTIND moved out from under us, or when
  // the argument we thought we were decoding isn't the current one (a fresh or
  // recursive `local OPTIND' invocation with a different argv).
  int cur_ai = optind - 1;
  bool arg_matches = cur_ai >= 0 && cur_ai < static_cast<int>(args.size()) &&
                     args[static_cast<size_t>(cur_ai)] == s_curarg;
  if (optind != s_optind || !arg_matches) { s_charidx = 1; s_optind = optind; }

  // bash clears OPTARG with unbind_variable_noref, which removes even a
  // readonly OPTARG (dropping the readonly attribute), so `${OPTARG-unset}'
  // reports it unset and a later re-declaration is not blocked.
  auto clear_optarg = [&]() { sh.unset("OPTARG", true); };
  auto badopt = [&](char c) {
    if (silent) { sh.set(name, "?"); sh.set("OPTARG", std::string(1, c)); }
    else {
      clear_optarg();
      sh.set(name, "?");
      std::fprintf(stderr, "%s: illegal option -- %c\n", sh.arg0.c_str(), c);
    }
  };
  auto needarg = [&](char c) {
    if (silent) { sh.set(name, ":"); sh.set("OPTARG", std::string(1, c)); }
    else {
      clear_optarg();
      sh.set(name, "?");
      std::fprintf(stderr, "%s: option requires an argument -- %c\n", sh.arg0.c_str(), c);
    }
  };

  for (;;) {
    int ai = optind - 1;  // 0-based index into args
    if (ai >= static_cast<int>(args.size())) { clear_optarg(); sh.set(name, "?"); return 1; }
    const std::string &arg = args[static_cast<size_t>(ai)];

    // Start of a fresh argument.
    if (s_charidx <= 1) {
      if (arg == "--") {  // end of options; consume it
        optind++;
        s_optind = optind;
        s_charidx = 1;
        sh.set("OPTIND", std::to_string(optind));
        clear_optarg();
        sh.set(name, "?");
        return 1;
      }
      if (arg.empty() || arg[0] != '-' || arg == "-") { clear_optarg(); sh.set(name, "?"); return 1; }
      s_charidx = 1;
      s_curarg = arg;
    }

    char c = (s_charidx < arg.size()) ? arg[s_charidx] : '\0';
    s_charidx++;
    bool last_char = s_charidx >= arg.size();
    if (last_char) { optind++; s_charidx = 1; }  // advance to the next argument

    if (c == ':' || optstring.find(c) == std::string::npos) {
      badopt(c);
      s_optind = optind;
      sh.set("OPTIND", std::to_string(optind));
      return 0;
    }
    size_t pos = optstring.find(c);
    if (pos + 1 < optstring.size() && optstring[pos + 1] == ':') {
      // Option takes an argument: rest of this arg, else the next arg.
      if (!last_char) {
        sh.set("OPTARG", arg.substr(s_charidx));
        optind++;
        s_charidx = 1;
      } else if (ai + 1 < static_cast<int>(args.size())) {
        sh.set("OPTARG", args[static_cast<size_t>(ai + 1)]);
        optind++;
      } else {
        needarg(c);
        s_optind = optind;
        sh.set("OPTIND", std::to_string(optind));
        return 0;
      }
    }
    sh.set(name, std::string(1, c));
    s_optind = optind;
    sh.set("OPTIND", std::to_string(optind));
    return 0;
  }
}

int signame_to_num(const std::string &s) {
  if (!s.empty() && std::isdigit(static_cast<unsigned char>(s[0]))) return std::atoi(s.c_str());
  std::string n = s;
  if (n.rfind("SIG", 0) == 0) n = n.substr(3);
  for (char &c : n) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  struct { const char *name; int sig; } tbl[] = {
      {"HUP", SIGHUP},   {"INT", SIGINT},   {"QUIT", SIGQUIT}, {"ILL", SIGILL},
      {"TRAP", SIGTRAP}, {"ABRT", SIGABRT}, {"FPE", SIGFPE},   {"KILL", SIGKILL},
      {"BUS", SIGBUS},   {"SEGV", SIGSEGV}, {"SYS", SIGSYS},   {"PIPE", SIGPIPE},
      {"ALRM", SIGALRM}, {"TERM", SIGTERM}, {"URG", SIGURG},   {"STOP", SIGSTOP},
      {"TSTP", SIGTSTP}, {"CONT", SIGCONT}, {"CHLD", SIGCHLD}, {"TTIN", SIGTTIN},
      {"TTOU", SIGTTOU}, {"XCPU", SIGXCPU}, {"XFSZ", SIGXFSZ}, {"VTALRM", SIGVTALRM},
      {"PROF", SIGPROF}, {"WINCH", SIGWINCH}, {"USR1", SIGUSR1}, {"USR2", SIGUSR2},
      {nullptr, 0}};
  for (int i = 0; tbl[i].name; i++)
    if (n == tbl[i].name) return tbl[i].sig;
  return SIGTERM;
}

int bi_kill(Shell &sh, const std::vector<std::string> &argv) {
  static const char *kUsage =
      "kill: usage: kill [-s sigspec | -n signum | -sigspec] pid | jobspec ... "
      "or kill -l [sigspec]\n";
  // Resolve a signal spec (a name like INT/SIGINT, or a number) to its number,
  // or -1 if it names no known signal.  signame_to_num() falls back to SIGTERM
  // for garbage, so a name is confirmed by round-tripping it back to canonical.
  auto resolve_sig = [](const std::string &spec) -> int {
    if (spec.empty()) return -1;
    if (std::isdigit(static_cast<unsigned char>(spec[0]))) {
      char *end = nullptr;
      long n = std::strtol(spec.c_str(), &end, 10);
      if (*end != '\0') return -1;
      return trapname_from_num(static_cast<int>(n)) ? static_cast<int>(n) : -1;
    }
    std::string n = spec;
    if (n.rfind("SIG", 0) == 0) n = n.substr(3);
    for (char &c : n) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    int num = signame_to_num(spec);
    const char *canon = trapname_from_num(num);
    return (canon && n == canon) ? num : -1;  // reject the SIGTERM fallback
  };

  // `kill -l [sigspec ...]' / `kill -L ...': list signal names and numbers.
  if (argv.size() > 1 && (argv[1] == "-l" || argv[1] == "-L")) {
    if (argv.size() == 2) { print_signal_list(); return 0; }  // list them all
    int st = 0;
    for (size_t k = 2; k < argv.size(); k++) {
      const std::string &a = argv[k];
      if (!a.empty() && std::isdigit(static_cast<unsigned char>(a[0]))) {
        int n = std::atoi(a.c_str());
        if (n > 128) n -= 128;  // a 128+signum exit status names the signal
        if (const char *nm = trapname_from_num(n)) std::printf("%s\n", nm);
        else {
          std::fprintf(stderr, "%skill: %s: invalid signal specification\n",
                       sh.err_prefix().c_str(), a.c_str());
          st = 1;
        }
      } else if (int n = resolve_sig(a); n > 0) {
        std::printf("%d\n", n);  // name -> number
      } else {
        std::fprintf(stderr, "%skill: %s: invalid signal specification\n",
                     sh.err_prefix().c_str(), a.c_str());
        st = 1;
      }
    }
    return st;
  }

  // Determine the signal from the options, and where the pid/jobspec list starts.
  int sig = SIGTERM;
  size_t i = 1;
  if (argv.size() > 1 && argv[1].size() > 1 && argv[1][0] == '-' && argv[1] != "--") {
    std::string opt = argv[1];
    if (opt == "-s" || opt == "-n") {
      // The signal is the next word: `-s TERM' / `-n 15'.
      if (argv.size() < 3) {
        std::fprintf(stderr, "%skill: %s: option requires an argument\n",
                     sh.err_prefix().c_str(), opt.c_str());
        return 1;
      }
      sig = resolve_sig(argv[2]);
      if (sig < 0) {
        std::fprintf(stderr, "%skill: %s: invalid signal specification\n",
                     sh.err_prefix().c_str(), argv[2].c_str());
        return 1;
      }
      i = 3;
    } else {
      // `-INT' / `-9' / `-SIGHUP': the spec is the option itself.
      sig = resolve_sig(opt.substr(1));
      if (sig < 0) {
        std::fprintf(stderr, "%skill: %s: invalid signal specification\n",
                     sh.err_prefix().c_str(), opt.substr(1).c_str());
        return 1;
      }
      i = 2;
    }
  } else if (argv.size() > 1 && argv[1] == "--") {
    i = 2;
  }
  // With no pid/jobspec operand, bash prints usage and fails with status 2.
  if (i >= argv.size()) { std::fputs(kUsage, stderr); return 2; }

  int st = 0;
  for (; i < argv.size(); i++) {
    const std::string &t = argv[i];
    pid_t target;
    if (!t.empty() && t[0] == '%') {
      Shell::Job *j = sh.job_by_spec(t);
      if (!j) {
        std::fprintf(stderr, "%skill: %s: no such job\n", sh.err_prefix().c_str(), t.c_str());
        st = 1;
        continue;
      }
      target = static_cast<pid_t>(-j->pgid);
    } else {
      // A pid must be a (possibly signed) decimal integer; anything else (an
      // empty word, `@12', `abc') is rejected WITHOUT signaling -- crucially,
      // atol("") is 0, and kill(0, sig) would signal the whole process group.
      size_t p = (t[0] == '+' || t[0] == '-') ? 1 : 0;
      bool numeric = p < t.size();
      for (; p < t.size(); p++)
        if (!std::isdigit(static_cast<unsigned char>(t[p]))) numeric = false;
      if (!numeric) {
        std::fprintf(stderr, "%skill: `%s': not a pid or valid job spec\n",
                     sh.err_prefix().c_str(), t.c_str());
        st = 1;
        continue;
      }
      target = static_cast<pid_t>(std::atol(t.c_str()));
    }
    if (kill(target, sig) != 0) {
      std::fprintf(stderr, "%skill: (%ld) - %s\n", sh.err_prefix().c_str(),
                   static_cast<long>(target), std::strerror(errno));
      st = 1;
    }
  }
  return st;
}

// ---- help (synopsis + short description derived from bash builtins/*.def) --
struct BuiltinHelp { const char *name, *synopsis, *shortdoc; };
static const BuiltinHelp kBuiltinHelp[] = {
    {":", ":", "Null command."},
    {"true", "true", "Return a successful result."},
    {"false", "false", "Return an unsuccessful result."},
    {"echo", "echo [-neE] [arg ...]", "Write arguments to the standard output."},
    {"printf", "printf [-v var] format [arguments]", "Formats and prints ARGUMENTS under control of the FORMAT."},
    {"pwd", "pwd [-LP]", "Print the name of the current working directory."},
    {"cd", "cd [-L|[-P [-e]]] [-@] [dir]", "Change the shell working directory."},
    {"export", "export [-fn] [name[=value] ...] or export -p [-f]", "Set export attribute for shell variables."},
    {"unset", "unset [-f] [-v] [-n] [name ...]", "Unset values and attributes of shell variables and functions."},
    {"set", "set [-abefhkmnptuvxBCEHPT] [-o option-name] [--] [-] [arg ...]", "Set or unset values of shell options and positional parameters."},
    {"read", "read [-Eers] [-a array] [-d delim] [-i text] [-n nchars] [-N nchars] [-p prompt] [-t timeout] [-u fd] [name ...]", "Read a line from the standard input and split it into fields."},
    {"test", "test [expr]", "Evaluate conditional expression."},
    {"shift", "shift [n]", "Shift positional parameters."},
    {"exit", "exit [n]", "Exit the shell."},
    {"return", "return [n]", "Return from a shell function."},
    {"break", "break [n]", "Exit for, while, or until loops."},
    {"continue", "continue [n]", "Resume for, while, or until loops."},
    {"eval", "eval [arg ...]", "Execute arguments as a shell command."},
    {"source", "source [-p path] filename [arguments]", "Execute commands from a file in the current shell."},
    {"local", "local [option] name[=value] ...", "Define local variables."},
    {"declare", "declare [-aAfFgiIlnrtux] [name[=value] ...] or declare -p [-aAfFilnrtux] [name ...]", "Set variable values and attributes."},
    {"typeset", "typeset [-aAfFgiIlnrtux] name[=value] ... or typeset -p [-aAfFilnrtux] [name ...]", "Set variable values and attributes."},
    {"readonly", "readonly [-aAf] [name[=value] ...] or readonly -p", "Mark shell variables as unchangeable."},
    {"let", "let arg [arg ...]", "Evaluate arithmetic expressions."},
    {"type", "type [-afptP] name [name ...]", "Display information about command type."},
    {"trap", "trap [-Plp] [[action] signal_spec ...]", "Trap signals and other events."},
    {"umask", "umask [-p] [-S] [mode]", "Display or set file mode mask."},
    {"getopts", "getopts optstring name [arg ...]", "Parse option arguments."},
    {"exec", "exec [-cl] [-a name] [command [argument ...]] [redirection ...]", "Replace the shell with the given command."},
    {"command", "command [-pVv] command [arg ...]", "Execute a simple command or display information about commands."},
    {"times", "times", "Display process times."},
    {"wait", "wait [-fn] [-p var] [id ...]", "Wait for job completion and return exit status."},
    {"jobs", "jobs [-lnprs] [jobspec ...] or jobs -x command [args]", "Display status of jobs."},
    {"fg", "fg [job_spec]", "Move job to the foreground."},
    {"bg", "bg [job_spec ...]", "Move jobs to the background."},
    {"disown", "disown [-h] [-ar] [jobspec ... | pid ...]", "Remove jobs from current shell."},
    {"kill", "kill [-s sigspec | -n signum | -sigspec] pid | jobspec ... or kill -l [sigspec]", "Send a signal to a job."},
    {"suspend", "suspend [-f]", "Suspend shell execution."},
    {"dirs", "dirs [-clpv] [+N] [-N]", "Display directory stack."},
    {"pushd", "pushd [-n] [+N | -N | dir]", "Add directories to stack."},
    {"popd", "popd [-n] [+N | -N]", "Remove directories from stack."},
    {"mapfile", "mapfile [-d delim] [-n count] [-O origin] [-s count] [-t] [-u fd] [-C callback] [-c quantum] [array]", "Read lines from the standard input into an indexed array variable."},
    {"readarray", "readarray [-d delim] [-n count] [-O origin] [-s count] [-t] [-u fd] [-C callback] [-c quantum] [array]", "Read lines from a file into an array variable."},
    {"help", "help [-dms] [pattern ...]", "Display information about builtin commands."},
    {"builtin", "builtin [shell-builtin [arg ...]]", "Execute shell builtins."},
    {"logout", "logout [n]", "Exit a login shell."},
    {"hash", "hash [-lr] [-p pathname] [-dt] [name ...]", "Remember or display program locations."},
    {"shopt", "shopt [-pqsu] [-o] [optname ...]", "Set and unset shell options."},
    {"ulimit", "ulimit [-SHabcdefiklmnpqrstuvxPRT] [limit]", "Modify shell resource limits."},
    {"enable", "enable [-a] [-dnps] [-f filename] [name ...]", "Enable and disable shell builtins."},
    {"caller", "caller [expr]", "Return the context of the current subroutine call."},
    {"alias", "alias [-p] [name[=value] ... ]", "Define or display aliases."},
    {"personality", "personality [-lLR] [{bash|zsh|sh|ksh|csh} [-c command]]", "Switch the shell's personality at runtime (zsh `emulate' syntax)."},
    {"unalias", "unalias [-a] name [name ...]", "Remove each NAME from the list of defined aliases."},
    {"history", "history [-c] [-d offset] [n] or history -anrw [filename] or history -ps arg [arg...]", "Display or manipulate the history list."},
    {"fc", "fc [-e ename] [-lnr] [first] [last] or fc -s [pat=rep] [command]", "Display or execute commands from the history list."},
    {"compgen", "compgen [-V varname] [-abcdefgjksuv] [-o option] [-A action] [-G globpat] [-W wordlist] [-F function] [-C command] [-X filterpat] [-P prefix] [-S suffix] [word]", "Display possible completions depending on the options."},
    {"complete", "complete [-abcdefgjksuv] [-pr] [-DEI] [-o option] [-A action] [-G globpat] [-W wordlist] [-F function] [-C command] [-X filterpat] [-P prefix] [-S suffix] [name ...]", "Specify how arguments are to be completed by Readline."},
    {"compopt", "compopt [-o|+o option] [-DEI] [name ...]", "Modify or display completion options."},
    {"bind", "bind [-lpsvPSVX] [-m keymap] [-f filename] [-q name] [-u name] [-r keyseq] [-x keyseq:shell-command] [keyseq:readline-function or readline-command]", "Set Readline key bindings and variables."},
    // Compound-command and special-syntax pseudo-builtins, so `help' lists them
    // like bash's shell_builtins[] (synopses verbatim from bash 5.3).
    {"!", "! PIPELINE", "Execute PIPELINE and negate its exit status."},
    {"%", "job_spec [&]", "Resume job in foreground."},
    {"(( ... ))", "(( expression ))", "Evaluate arithmetic expression."},
    {".", ". [-p path] filename [arguments]", "Execute commands from a file in the current shell."},
    {"[", "[ arg... ]", "Evaluate conditional expression."},
    {"[[ ... ]]", "[[ expression ]]", "Execute conditional command."},
    {"{ ... }", "{ COMMANDS ; }", "Group commands as a unit."},
    {"case", "case WORD in [PATTERN [| PATTERN]...) COMMANDS ;;]... esac", "Execute commands based on pattern matching."},
    {"coproc", "coproc [NAME] command [redirections]", "Create a coprocess named NAME."},
    {"for", "for NAME [in WORDS ... ] ; do COMMANDS; done", "Execute commands for each member in a list."},
    {"for ((", "for (( exp1; exp2; exp3 )); do COMMANDS; done", "Arithmetic for loop."},
    {"function", "function name { COMMANDS ; } or name () { COMMANDS ; }", "Define shell function."},
    {"if", "if COMMANDS; then COMMANDS; [ elif COMMANDS; then COMMANDS; ]... [ else COMMANDS; ] fi", "Execute commands based on conditional."},
    {"select", "select NAME [in WORDS ... ;] do COMMANDS; done", "Select words from a list and execute commands."},
    {"time", "time [-p] pipeline", "Report time consumed by pipeline's execution."},
    {"until", "until COMMANDS; do COMMANDS-2; done", "Execute commands as long as a test does not succeed."},
    {"variables", "variables - Names and meanings of some shell variables", "Common shell variable names and usage."},
    {"while", "while COMMANDS; do COMMANDS-2; done", "Execute commands as long as a test succeeds."},
};

int bi_help(Shell &sh, const std::vector<std::string> &argv) {
  bool dflag = false, sflag = false;
  size_t i = 1;
  for (; i < argv.size(); i++) {
    const std::string &a = argv[i];
    if (a == "--") { i++; break; }
    if (a.size() < 2 || a[0] != '-') break;
    for (size_t k = 1; k < a.size(); k++) {
      if (a[k] == 'd') dflag = true;
      else if (a[k] == 's') sflag = true;
      else if (a[k] == 'm') { /* man format: accepted */ }
    }
  }
  if (i >= argv.size()) {
    std::printf("gnash, version %s\n", sh.get("BASH_VERSION").c_str());
    std::printf("These shell commands are defined internally.  Type `help' to see this list.\n");
    std::printf("Type `help name' to find out more about the function `name'.\n");
    std::printf("Use `info bash' to find out more about the shell in general.\n");
    std::printf("Use `man -k' or `info' to find out more about commands not in this list.\n\n");
    std::printf("A star (*) next to a name means that the command is disabled.\n\n");
    // The `personality' pseudo-builtin is gnash-specific; leave it out of the
    // listing so the two-column pairing matches bash's shell_builtins[] exactly.
    std::vector<const BuiltinHelp *> items;
    for (const auto &h : kBuiltinHelp)
      if (std::strcmp(h.name, "personality") != 0) items.push_back(&h);
    std::sort(items.begin(), items.end(),
              [](const BuiltinHelp *a, const BuiltinHelp *b) { return std::strcmp(a->name, b->name) < 0; });
    // bash's dispcolumn(): a two-column layout of width 40.  Each cell is a
    // leading space (`*' if disabled -- never here) then the synopsis; when it
    // is too long it is truncated and a `>' marks the cut.  The first column is
    // padded with spaces to the column width; the second is not.
    // bash's dispcolumn(): two columns of field width 40.  A cell is a leading
    // marker (space; `*' if disabled -- never here) then the synopsis, cut with
    // a trailing `>' when too long.  Matching bash byte-for-byte, the first
    // column shows up to 37 synopsis chars and is padded to 40; the second up
    // to 35.
    const size_t kWidth = 40, kCol1 = 37, kCol2 = 36;
    size_t height = (items.size() + 1) / 2;
    auto cell = [](const std::string &syn, size_t cap) {
      std::string s = " ";
      if (syn.size() > cap) s += syn.substr(0, cap) + ">";  // truncated
      else s += syn;
      return s;
    };
    for (size_t r = 0; r < height; r++) {
      std::string left = cell(items[r]->synopsis, kCol1);
      if (r + height >= items.size()) { std::printf("%s\n", left.c_str()); continue; }
      std::printf("%s", left.c_str());
      for (size_t j = left.size(); j < kWidth; j++) std::putchar(' ');
      std::printf("%s\n", cell(items[r + height]->synopsis, kCol2).c_str());
    }
    return 0;
  }

  int st = 0;
  for (; i < argv.size(); i++) {
    const std::string &pat = argv[i];
    std::vector<const BuiltinHelp *> hits;
    for (const auto &h : kBuiltinHelp)
      if (strmatch(const_cast<char *>(pat.c_str()), const_cast<char *>(h.name), 0) == 0)
        hits.push_back(&h);
    if (hits.empty())  // fall back to a prefix match
      for (const auto &h : kBuiltinHelp)
        if (std::strncmp(h.name, pat.c_str(), pat.size()) == 0) hits.push_back(&h);
    if (hits.empty()) {
      std::fflush(stdout);
      std::fprintf(stderr,
                   "%shelp: no help topics match `%s'.  Try `help help' or `man -k %s' or `info %s'.\n",
                   sh.err_prefix().c_str(), pat.c_str(), pat.c_str(), pat.c_str());
      st = 1;
      continue;
    }
    for (const BuiltinHelp *h : hits) {
      if (sflag) std::printf("%s: %s\n", h->name, h->synopsis);
      else if (dflag) std::printf("%s - %s\n", h->name, h->shortdoc);
      else std::printf("%s: %s\n    %s\n", h->name, h->synopsis, h->shortdoc);
    }
  }
  return st;
}

int bi_builtin(Shell &sh, const std::vector<std::string> &argv) {
  if (argv.size() < 2) return 0;
  size_t i = 1;
  if (argv[i] == "--") i++;  // `builtin' takes no options besides `--'
  else if (argv[i].size() >= 2 && argv[i][0] == '-') {
    std::fprintf(stderr, "%sbuiltin: %s: invalid option\n", sh.err_prefix().c_str(),
                 argv[i].c_str());
    std::fprintf(stderr, "builtin: usage: builtin [shell-builtin [arg ...]]\n");
    return 2;
  }
  if (i >= argv.size()) return 0;
  std::vector<std::string> sub(argv.begin() + i, argv.end());
  if (!is_builtin_name(sub[0])) {
    std::fflush(stdout);
    std::fprintf(stderr, "%sbuiltin: %s: not a shell builtin\n", sh.err_prefix().c_str(),
                 sub[0].c_str());
    return 1;
  }
  int st = 0;
  run_builtin(sh, sub, &st);
  return st;
}

int bi_logout(Shell &sh, const std::vector<std::string> &argv) {
  if (!sh.login_shell) {
    std::fflush(stdout);
    std::fprintf(stderr, "%slogout: not login shell: use `exit'\n", sh.err_prefix().c_str());
    return 1;
  }
  sh.exiting = true;
  sh.exit_status = argv.size() > 1 ? (std::atoi(argv[1].c_str()) & 0xff) : sh.last_status;
  return sh.exit_status;
}

int bi_hash(Shell &sh, const std::vector<std::string> &argv) {
  bool list_l = false, del_d = false, print_t = false;
  std::string ppath;
  size_t i = 1;
  for (; i < argv.size(); i++) {
    const std::string &a = argv[i];
    if (a == "--") { i++; break; }
    if (a.size() < 2 || a[0] != '-') break;
    bool consumed = false;
    for (size_t k = 1; k < a.size(); k++) {
      char o = a[k];
      if (o == 'r') { sh.hashed.clear(); }
      else if (o == 'l') list_l = true;
      else if (o == 'd') del_d = true;
      else if (o == 't') print_t = true;
      else if (o == 'p') {
        ppath = (k + 1 < a.size()) ? a.substr(k + 1) : (i + 1 < argv.size() ? argv[++i] : "");
        consumed = true;
        break;
      }
    }
    if (consumed) continue;
  }
  if (!ppath.empty() && i < argv.size()) {
    // Under a restricted shell, a hashed pathname may not contain `/', and a
    // relative one cannot be resolved, so neither can be hashed.
    if (sh.opt_restricted) {
      if (ppath.find('/') != std::string::npos)
        std::fprintf(stderr, "%shash: %s: restricted\n", sh.err_prefix().c_str(),
                     ppath.c_str());
      else
        std::fprintf(stderr, "%shash: %s: not found\n", sh.err_prefix().c_str(),
                     ppath.c_str());
      return 1;
    }
    sh.hashed[argv[i]] = ppath;
    return 0;
  }
  if (i >= argv.size()) {
    if (sh.hashed.empty()) {
      if (!list_l) std::printf("hash: hash table empty\n");
      return 0;
    }
    if (list_l)
      for (const auto &kv : sh.hashed)
        std::printf("builtin hash -p %s %s\n", kv.second.c_str(), kv.first.c_str());
    else {
      std::printf("hits\tcommand\n");
      for (const auto &kv : sh.hashed) std::printf("%4d\t%s\n", 0, kv.second.c_str());
    }
    return 0;
  }
  int st = 0;
  for (; i < argv.size(); i++) {
    const std::string &n = argv[i];
    if (del_d) {  // `hash -d NAME': error if NAME is not hashed
      if (sh.hashed.erase(n) == 0) {
        std::fflush(stdout);
        std::fprintf(stderr, "%shash: %s: not found\n", sh.err_prefix().c_str(), n.c_str());
        st = 1;
      }
      continue;
    }
    if (print_t) {
      auto it = sh.hashed.find(n);
      if (it != sh.hashed.end()) std::printf("%s\n", it->second.c_str());
      else { std::fflush(stdout); std::fprintf(stderr, "%shash: %s: not found\n", sh.err_prefix().c_str(), n.c_str()); st = 1; }
      continue;
    }
    std::string p = find_in_path(sh, n);
    if (p.empty()) { std::fflush(stdout); std::fprintf(stderr, "%shash: %s: not found\n", sh.err_prefix().c_str(), n.c_str()); st = 1; }
    else sh.hashed[n] = p;
  }
  return st;
}

static const struct { const char *name; bool on; } kShoptDefaults[] = {
    {"array_expand_once", false}, {"assoc_expand_once", false}, {"autocd", false},
    {"bash_source_fullpath", false}, {"cdable_vars", false}, {"cdspell", false},
    {"checkhash", false}, {"checkjobs", false}, {"checkwinsize", true}, {"cmdhist", true},
    {"compat31", false}, {"compat32", false}, {"compat40", false}, {"compat41", false},
    {"compat42", false}, {"compat43", false}, {"compat44", false}, {"complete_fullquote", true},
    {"direxpand", false}, {"dirspell", false}, {"dotglob", false}, {"execfail", false},
    {"expand_aliases", false}, {"extdebug", false}, {"extglob", false}, {"extquote", true},
    {"failglob", false}, {"force_fignore", true}, {"globasciiranges", true}, {"globskipdots", true},
    {"globstar", false}, {"gnu_errfmt", false}, {"histappend", false}, {"histreedit", false},
    {"histverify", false}, {"hostcomplete", true}, {"huponexit", false}, {"inherit_errexit", false},
    {"interactive_comments", true}, {"lastpipe", false}, {"lithist", false}, {"localvar_inherit", false},
    {"localvar_unset", false}, {"login_shell", false}, {"mailwarn", false},
    {"no_empty_cmd_completion", false}, {"nocaseglob", false}, {"nocasematch", false},
    {"noexpand_translation", false}, {"nullglob", false}, {"patsub_replacement", true},
    {"progcomp", true}, {"progcomp_alias", false}, {"promptvars", true}, {"restricted_shell", false},
    {"shift_verbose", false}, {"sourcepath", true}, {"varredir_close", false}, {"xpg_echo", false},
};

}  // namespace (close the anonymous namespace so shopt_seed has external linkage)

void shopt_seed(Shell &sh) {
  if (!sh.shopt_opts.empty()) return;
  for (const auto &o : kShoptDefaults) sh.shopt_opts[o.name] = o.on;
  sh.shopt_opts["login_shell"] = sh.login_shell;
}

namespace {  // reopen the anonymous namespace

int bi_shopt(Shell &sh, const std::vector<std::string> &argv) {
  shopt_seed(sh);
  bool set_s = false, unset_u = false, quiet_q = false, print_p = false;
  bool o_names = false;  // -o: names are `set -o' option names
  size_t i = 1;
  for (; i < argv.size(); i++) {
    const std::string &a = argv[i];
    if (a == "--") { i++; break; }
    if (a.size() < 2 || a[0] != '-') break;
    for (size_t k = 1; k < a.size(); k++) {
      switch (a[k]) {
        case 's': set_s = true; break;
        case 'u': unset_u = true; break;
        case 'q': quiet_q = true; break;
        case 'p': print_p = true; break;
        case 'o': o_names = true; break;
        default:
          std::fprintf(stderr, "%sshopt: -%c: invalid option\n",
                       sh.err_prefix().c_str(), a[k]);
          std::fprintf(stderr, "shopt: usage: shopt [-pqsu] [-o] [optname ...]\n");
          return 2;
      }
    }
  }

  if (o_names) {
    // `-p' reproduces as `set -o'/`set +o' commands; otherwise a NAME<TAB>state
    // two-column listing (bash's list_minus_o_opts, field width 15).
    auto show_o = [&](const std::string &n, bool on) {
      if (quiet_q) return;
      if (print_p) std::printf("set %co %s\n", on ? '-' : '+', n.c_str());
      else std::printf("%-15s\t%s\n", n.c_str(), on ? "on" : "off");
    };
    if (i >= argv.size()) {  // list set -o options
      for (const auto &o : set_option_states(sh)) {
        if (set_s && !o.second) continue;
        if (unset_u && o.second) continue;
        show_o(o.first, o.second);
      }
      return 0;
    }
    int st = 0;
    for (; i < argv.size(); i++) {
      const std::string &n = argv[i];
      bool found = false, cur = false;
      for (const auto &o : set_option_states(sh))
        if (o.first == n) { found = true; cur = o.second; }
      if (!found) {
        if (!quiet_q)
          std::fprintf(stderr, "%sshopt: %s: invalid option name\n",
                       sh.err_prefix().c_str(), n.c_str());
        st = 1;
        continue;
      }
      if (set_s || unset_u) set_o_option(sh, n, set_s);
      else if (quiet_q) { if (!cur) st = 1; }
      else show_o(n, cur);
    }
    return st;
  }
  auto show = [&](const std::string &n, bool on) {
    if (quiet_q) return;
    if (print_p) std::printf("shopt -%c %s\n", on ? 's' : 'u', n.c_str());
    else std::printf("%-20s\t%s\n", n.c_str(), on ? "on" : "off");
  };

  if (i >= argv.size()) {  // list (optionally only -s or -u subset)
    for (const auto &kv : sh.shopt_opts) {
      if (set_s && !kv.second) continue;
      if (unset_u && kv.second) continue;
      show(kv.first, kv.second);
    }
    return 0;
  }

  int st = 0;
  for (; i < argv.size(); i++) {
    const std::string &n = argv[i];
    auto it = sh.shopt_opts.find(n);
    if (it == sh.shopt_opts.end()) {
      if (!quiet_q) {
        std::fflush(stdout);
        std::fprintf(stderr, "%sshopt: %s: invalid shell option name\n", sh.err_prefix().c_str(), n.c_str());
      }
      st = 1;
      continue;
    }
    if (set_s) it->second = true;
    else if (unset_u) it->second = false;
    else {  // query
      show(it->first, it->second);
      if (!it->second) st = 1;
    }
  }
  sh.opt_extdebug = sh.shopt_opts["extdebug"];  // gates BASH_ARGC/BASH_ARGV
  return st;
}

// ---- ulimit --------------------------------------------------------------
struct UlimitRes { char opt; int res; long factor; const char *desc; const char *unit; };
static const UlimitRes kUlimits[] = {
    {'c', RLIMIT_CORE, 1024, "core file size", "blocks"},
    {'d', RLIMIT_DATA, 1024, "data seg size", "kbytes"},
    {'f', RLIMIT_FSIZE, 1024, "file size", "blocks"},
    {'l', RLIMIT_MEMLOCK, 1024, "max locked memory", "kbytes"},
    {'m', RLIMIT_RSS, 1024, "max memory size", "kbytes"},
    {'n', RLIMIT_NOFILE, 1, "open files", nullptr},
    {'p', -1, 512, "pipe size", "512 bytes"},  // no real rlimit; bash reports 1
    {'s', RLIMIT_STACK, 1024, "stack size", "kbytes"},
    {'t', RLIMIT_CPU, 1, "cpu time", "seconds"},
    {'u', RLIMIT_NPROC, 1, "max user processes", nullptr},
#ifdef RLIMIT_AS
    {'v', RLIMIT_AS, 1024, "virtual memory", "kbytes"},
#endif
};

// Print one resource limit.  When `labeled' is false (a single resource being
// reported), bash prints just the value; otherwise the aligned descriptive line.
static void ulimit_print_one(const UlimitRes &r, bool hard, bool labeled) {
  std::string val;
  if (r.res < 0) {
    val = "1";  // pipe size
  } else {
    struct rlimit rl;
    if (getrlimit(r.res, &rl) != 0) return;
    rlim_t v = hard ? rl.rlim_max : rl.rlim_cur;
    if (v == RLIM_INFINITY) val = "unlimited";
    else val = std::to_string(static_cast<unsigned long long>(v) / static_cast<unsigned long long>(r.factor));
  }
  if (!labeled) { std::printf("%s\n", val.c_str()); return; }
  char unitstr[32];
  if (r.unit) std::snprintf(unitstr, sizeof unitstr, "(%s, -%c) ", r.unit, r.opt);
  else std::snprintf(unitstr, sizeof unitstr, "(-%c) ", r.opt);
  std::printf("%-20s %20s%s\n", r.desc, unitstr, val.c_str());
}

int bi_ulimit(Shell &sh, const std::vector<std::string> &argv) {
  bool hard = false, soft = false, all = false;
  std::vector<char> reqs;  // requested resources, in order
  std::string value;
  size_t i = 1;
  for (; i < argv.size(); i++) {
    const std::string &a = argv[i];
    if (a == "--") { i++; break; }
    if (a.size() < 2 || (a[0] != '-' && a[0] != '+')) break;
    for (size_t k = 1; k < a.size(); k++) {
      char o = a[k];
      if (o == 'H') hard = true;
      else if (o == 'S') soft = true;
      else if (o == 'a') all = true;
      else reqs.push_back(o);
    }
  }
  if (i < argv.size()) value = argv[i];
  if (!hard && !soft) soft = true;  // default acts on the soft limit

  if (all) {
    for (const auto &r : kUlimits) ulimit_print_one(r, hard, true);
    return 0;
  }
  if (reqs.empty()) reqs.push_back('f');  // default resource

  // Resolve every requested resource up front (reject an unknown one).
  std::vector<const UlimitRes *> rs;
  for (char o : reqs) {
    const UlimitRes *r = nullptr;
    for (const auto &e : kUlimits) if (e.opt == o) { r = &e; break; }
    if (!r) { std::fprintf(stderr, "%sulimit: -%c: invalid option\n", sh.err_prefix().c_str(), o); return 2; }
    rs.push_back(r);
  }

  if (value.empty()) {  // report: value-only for a single resource, labeled for several
    bool labeled = rs.size() > 1;
    for (const UlimitRes *r : rs) ulimit_print_one(*r, hard, labeled);
    return 0;
  }
  // Set the given value on each requested resource.
  for (const UlimitRes *r : rs) {
    if (r->res < 0) continue;  // pipe size is not settable
    struct rlimit rl;
    getrlimit(r->res, &rl);
    rlim_t nv;
    if (value == "unlimited") nv = RLIM_INFINITY;
    else if (value == "hard") nv = rl.rlim_max;
    else if (value == "soft") nv = rl.rlim_cur;
    else nv = static_cast<rlim_t>(std::strtoull(value.c_str(), nullptr, 10) * static_cast<unsigned long long>(r->factor));
    if (hard) rl.rlim_max = nv;
    if (soft) rl.rlim_cur = nv;
    if (setrlimit(r->res, &rl) != 0) {
      std::fprintf(stderr, "%sulimit: %s\n", sh.err_prefix().c_str(), std::strerror(errno));
      return 1;
    }
  }
  return 0;
}

// ---- enable --------------------------------------------------------------
int bi_enable(Shell &sh, const std::vector<std::string> &argv) {
  // The POSIX special builtins, listed by `enable -s'.
  static const std::set<std::string> kSpecial = {
      ".",      ":",    "break", "continue", "eval",  "exec",
      "exit",   "export", "readonly", "return", "set", "shift",
      "source", "times", "trap",  "unset"};
  bool disable = false, all = false, special = false;
  size_t i = 1;
  for (; i < argv.size(); i++) {
    const std::string &a = argv[i];
    if (a == "--") { i++; break; }
    if (a.size() < 2 || a[0] != '-') break;
    for (size_t k = 1; k < a.size(); k++) {
      if (a[k] == 'n') disable = true;
      else if (a[k] == 'a') all = true;
      else if (a[k] == 's') special = true;
      else if (a[k] == 'p') { /* posix reusable list: like default */ }
    }
  }
  if (i >= argv.size()) {  // list
    for (const std::string &nm : builtin_names_sorted()) {
      if (special && !kSpecial.count(nm)) continue;
      bool off = sh.disabled_builtins.count(nm) != 0;
      if (all) std::printf("enable %s%s\n", off ? "-n " : "", nm.c_str());
      else if (disable) { if (off) std::printf("enable -n %s\n", nm.c_str()); }
      else if (!off) std::printf("enable %s\n", nm.c_str());
    }
    return 0;
  }
  int st = 0;
  for (; i < argv.size(); i++) {
    const std::string &n = argv[i];
    if (!is_builtin_name(n)) {
      std::fprintf(stderr, "%senable: %s: not a shell builtin\n", sh.err_prefix().c_str(), n.c_str());
      st = 1;
      continue;
    }
    if (disable) sh.disabled_builtins.insert(n);
    else sh.disabled_builtins.erase(n);
  }
  return st;
}

// ---- caller --------------------------------------------------------------
int bi_caller(Shell &sh, const std::vector<std::string> &argv) {
  // Mirror bash's caller.def: read the BASH_LINENO / BASH_SOURCE / FUNCNAME
  // call-context arrays so top-level ("line source", with NULL for a missing
  // frame) and the error diagnostics match byte-for-byte.
  static const char *kUsage = "caller: usage: caller [expr]\n";
  size_t ai = 1;
  // no_options: a leading `-X' (anything but a bare `--') is an invalid option.
  if (ai < argv.size() && argv[ai].size() >= 1 && argv[ai][0] == '-' &&
      argv[ai] != "--") {
    std::fprintf(stderr, "%scaller: %s: invalid option\n",
                 sh.err_prefix().c_str(), argv[ai].c_str());
    std::fprintf(stderr, "%s", kUsage);
    return 2;
  }
  if (ai < argv.size() && argv[ai] == "--") ai++;

  std::vector<std::string> lineno = sh.array_values("BASH_LINENO");
  std::vector<std::string> source = sh.array_values("BASH_SOURCE");
  std::vector<std::string> funcs = sh.array_values("FUNCNAME");
  auto at = [](const std::vector<std::string> &v, size_t i) -> const char * {
    return i < v.size() ? v[i].c_str() : "NULL";
  };
  if (lineno.empty() || source.empty()) return 1;

  if (ai >= argv.size()) {  // caller (no arg): "line source"
    std::printf("%s %s\n", at(lineno, 0), at(source, 1));
    return 0;
  }
  // valid_number: optional surrounding blanks, an optional sign, then digits.
  const std::string &w = argv[ai];
  size_t p = 0, e = w.size();
  while (p < e && std::isspace(static_cast<unsigned char>(w[p]))) p++;
  while (e > p && std::isspace(static_cast<unsigned char>(w[e - 1]))) e--;
  size_t d = p;
  if (d < e && (w[d] == '+' || w[d] == '-')) d++;
  bool valid = d < e;
  for (size_t k = d; k < e; k++)
    if (!std::isdigit(static_cast<unsigned char>(w[k]))) { valid = false; break; }
  if (!valid) {
    std::fprintf(stderr, "%scaller: %s: invalid number\n",
                 sh.err_prefix().c_str(), w.c_str());
    std::fprintf(stderr, "%s", kUsage);
    return 2;
  }
  // A valid EXPR requires an active function frame and an in-range index.
  if (funcs.empty()) return 1;
  long num = std::atol(w.substr(p, e - p).c_str());
  size_t un = static_cast<size_t>(num);
  if (num < 0 || un >= lineno.size() || un + 1 >= source.size() ||
      un + 1 >= funcs.size())
    return 1;
  std::printf("%s %s %s\n", lineno[un].c_str(), funcs[un + 1].c_str(),
              source[un + 1].c_str());
  return 0;
}

// ---- alias / unalias -----------------------------------------------------
static std::string alias_quote(const std::string &v) {
  std::string r = "'";
  for (char c : v) { if (c == '\'') r += "'\\''"; else r += c; }
  return r + "'";
}

int bi_alias(Shell &sh, const std::vector<std::string> &argv) {
  size_t i = 1;
  char kind = 0;  // 0 = regular; 'g' = global, 's' = suffix (zsh `alias -g'/`-s')
  while (i < argv.size() && argv[i].size() >= 2 && argv[i][0] == '-' && argv[i] != "--") {
    if (argv[i] == "-p") { i++; continue; }
    if (sh.is_zsh() && argv[i] == "-g") { kind = 'g'; i++; continue; }
    if (sh.is_zsh() && argv[i] == "-s") { kind = 's'; i++; continue; }
    std::fprintf(stderr, "%salias: %s: invalid option\n", sh.err_prefix().c_str(),
                 argv[i].c_str());
    std::fprintf(stderr, "alias: usage: alias [-p] [name[=value] ... ]\n");
    return 2;
  }
  if (i < argv.size() && argv[i] == "--") i++;
  auto &table = (kind == 'g') ? sh.global_aliases : (kind == 's') ? sh.suffix_aliases : sh.aliases;
  // Regular aliases list bash-style (`alias name=value'); zsh's global/suffix
  // list as `name=value'.
  auto show = [&](const std::string &n, const std::string &v) {
    if (kind == 0)
      std::printf("alias %s=%s\n", n.c_str(), alias_quote(v).c_str());
    else
      std::printf("%s=%s\n", n.c_str(), alias_quote(v).c_str());
  };
  if (i >= argv.size()) {
    for (const auto &kv : table) show(kv.first, kv.second);
    return 0;
  }
  int st = 0;
  for (; i < argv.size(); i++) {
    const std::string &a = argv[i];
    auto eq = a.find('=');
    if (eq == std::string::npos) {
      auto it = table.find(a);
      if (it != table.end())
        show(it->first, it->second);
      else {
        std::fflush(stdout);
        std::fprintf(stderr, "%salias: %s: not found\n", sh.err_prefix().c_str(), a.c_str());
        st = 1;
      }
    } else {
      table[a.substr(0, eq)] = a.substr(eq + 1);
    }
  }
  return st;
}

int bi_unalias(Shell &sh, const std::vector<std::string> &argv) {
  // zsh's unalias has `-a' (all) and `-s' (suffix aliases), but NOT `-g': a
  // global alias is removed by plain `unalias name' (regular and global share
  // the name lookup).
  size_t i = 1;
  bool all = false, suffix = false;
  while (i < argv.size() && argv[i].size() >= 2 && argv[i][0] == '-' && argv[i] != "--") {
    if (argv[i] == "-a") { all = true; i++; continue; }
    if (sh.is_zsh() && argv[i] == "-s") { suffix = true; i++; continue; }
    std::fprintf(stderr, "%sunalias: %s: invalid option\n", sh.err_prefix().c_str(),
                 argv[i].c_str());
    std::fprintf(stderr, "unalias: usage: unalias [-a] name [name ...]\n");
    return 2;
  }
  if (all) {
    sh.aliases.clear();
    sh.global_aliases.clear();
    sh.suffix_aliases.clear();
    return 0;
  }
  int st = 0;
  for (; i < argv.size(); i++) {
    bool removed;
    if (suffix) {
      removed = sh.suffix_aliases.erase(argv[i]) > 0;
    } else {
      bool r = sh.aliases.erase(argv[i]) > 0;         // remove from both regular
      bool g = sh.global_aliases.erase(argv[i]) > 0;  // and global (they may coexist)
      removed = r || g;
    }
    if (!removed) {
      std::fprintf(stderr, "%sunalias: %s: not found\n", sh.err_prefix().c_str(), argv[i].c_str());
      st = 1;
    }
  }
  return st;
}

// ---- history / fc --------------------------------------------------------
static std::string hist_file(Shell &sh) {
  std::string h = sh.get("HISTFILE");
  if (!h.empty()) return h;
  const char *home = std::getenv("HOME");
  return home ? std::string(home) + "/.gnash_history" : std::string();
}

int bi_history(Shell &sh, const std::vector<std::string> &argv) {
  static const char *kUsage =
      "history: usage: history [-c] [-d offset] [n] or history -anrw [filename] "
      "or history -ps arg [arg...]";

  // Parse a history display number (possibly negative = from the end) into a
  // 0-based list index; false if out of range.
  auto resolve = [&](long v, int &idx) {
    long n = v < 0 ? history_length + v : v - history_base;
    if (n < 0 || n >= history_length) return false;
    idx = static_cast<int>(n);
    return true;
  };

  bool clear = false, del = false, app = false, wr = false, rd = false, nw = false;
  bool stash = false, expand = false;
  std::string del_arg, fname;
  int limit = -1;
  size_t i = 1;
  std::vector<std::string> rest;

  for (; i < argv.size(); i++) {
    const std::string &a = argv[i];
    if (a == "--") { i++; break; }
    if (a.size() >= 2 && a[0] == '-' &&
        !std::isdigit(static_cast<unsigned char>(a[1]))) {
      for (size_t k = 1; k < a.size(); k++) {
        switch (a[k]) {
          case 'c': clear = true; break;
          case 'd':
            del = true;
            if (k + 1 < a.size()) { del_arg = a.substr(k + 1); k = a.size(); }
            else if (i + 1 < argv.size()) del_arg = argv[++i];
            break;
          case 'a': app = true; break;
          case 'w': wr = true; break;
          case 'r': rd = true; break;
          case 'n': nw = true; break;
          case 's': stash = true; break;
          case 'p': expand = true; break;
          default:
            std::fprintf(stderr, "%shistory: -%c: invalid option\n",
                         sh.err_prefix().c_str(), a[k]);
            std::fprintf(stderr, "%s\n", kUsage);
            return 2;
        }
      }
    } else {
      break;
    }
  }
  for (; i < argv.size(); i++) rest.push_back(argv[i]);

  if (app + wr + rd + nw > 1) {
    std::fprintf(stderr, "%shistory: cannot use more than one of -anrw\n",
                 sh.err_prefix().c_str());
    return 2;
  }

  if (clear) {
    clear_history();
    sh.hist_new_entries = 0;
    if (!del && !app && !wr && !rd && !nw && !stash && !expand && rest.empty()) return 0;
  }
  if (del) {
    // OFFSET or START-END ranges, in history display numbers; negative counts
    // from the end.  bash's error messages distinguish an unparsable token
    // ("invalid number") from a parsable but out-of-range one.
    const std::string &d = del_arg;
    char *e1 = nullptr;
    long v1 = std::strtol(d.c_str(), &e1, 10);
    int lo, hi;
    if (e1 != d.c_str() && *e1 == '\0') {           // single offset
      if (!resolve(v1, lo)) {
        std::fprintf(stderr, "%shistory: %s: history position out of range\n",
                     sh.err_prefix().c_str(), d.c_str());
        return 2;
      }
      HIST_ENTRY *old = remove_history(lo);
      if (old) free_history_entry(old);
      return 0;
    }
    if (e1 != d.c_str() && *e1 == '-') {             // START-END range
      const char *p2 = e1 + 1;
      char *e2 = nullptr;
      long v2 = std::strtol(p2, &e2, 10);
      if (e2 != p2 && *e2 == '\0') {
        if (!resolve(v1, lo)) {
          std::fprintf(stderr, "%shistory: %ld: history position out of range\n",
                       sh.err_prefix().c_str(), v1);
          return 2;
        }
        if (!resolve(v2, hi)) {
          std::fprintf(stderr, "%shistory: %ld: history position out of range\n",
                       sh.err_prefix().c_str(), v2);
          return 2;
        }
        if (hi < lo) { int t = lo; lo = hi; hi = t; }
        for (int k = hi; k >= lo; k--) {
          HIST_ENTRY *old = remove_history(k);
          if (old) free_history_entry(old);
        }
        return 0;
      }
      // Range-shaped but the end does not parse: report the whole token.
      std::fprintf(stderr, "%shistory: %s: history position out of range\n",
                   sh.err_prefix().c_str(), d.c_str());
      return 2;
    }
    std::fprintf(stderr, "%shistory: %s: invalid number\n", sh.err_prefix().c_str(),
                 d.c_str());
    return 2;
  }
  if (stash) {
    std::string line = join(rest, 0);
    if (!line.empty()) { add_history(line.c_str()); sh.hist_new_entries++; }
    return 0;
  }
  if (expand) {
    int st = 0;
    sh.sync_histchars();
    for (const std::string &a : rest) {
      char *e = nullptr;
      int hr = history_expand(const_cast<char *>(a.c_str()), &e);
      if (hr < 0) {
        std::fprintf(stderr, "%shistory: %s: history expansion failed\n",
                     sh.err_prefix().c_str(), a.c_str());
        st = 1;
      } else if (e) {
        std::printf("%s\n", e);
      }
      std::free(e);
    }
    return st;
  }
  history_write_timestamps = sh.is_set("HISTTIMEFORMAT") ? 1 : 0;
  history_multiline_entries = history_write_timestamps;
  if (wr) { write_history((rest.empty() ? hist_file(sh) : rest[0]).c_str()); return 0; }
  if (app) {
    append_history(sh.hist_new_entries, (rest.empty() ? hist_file(sh) : rest[0]).c_str());
    sh.hist_new_entries = 0;
    return 0;
  }
  if (rd || nw) { read_history((rest.empty() ? hist_file(sh) : rest[0]).c_str()); return 0; }

  if (!rest.empty()) {
    char *e = nullptr;
    long v = std::strtol(rest[0].c_str(), &e, 10);
    if (e == rest[0].c_str() || *e != '\0' || v < 0) {
      std::fprintf(stderr, "%shistory: %s: numeric argument required\n",
                   sh.err_prefix().c_str(), rest[0].c_str());
      return 2;
    }
    limit = static_cast<int>(v);
  }

  HIST_ENTRY **list = history_list();
  if (!list) return 0;
  int n = 0;
  while (list[n]) n++;
  int start = (limit >= 0 && limit < n) ? n - limit : 0;
  for (int k = start; k < n; k++)
    std::printf("%5d  %s\n", history_base + k, list[k]->line);
  return 0;
}

int bi_fc(Shell &sh, const std::vector<std::string> &argv) {
  static const char *kUsage =
      "fc: usage: fc [-e ename] [-lnr] [first] [last] or fc -s [pat=rep] [command]";
  enum { kInvalid = -2, kNotFound = -3 };

  bool list = false, nonum = false, reverse = false, subst = false;
  std::string ename;
  bool have_e = false;
  size_t i = 1;
  for (; i < argv.size(); i++) {
    const std::string &a = argv[i];
    if (a.size() < 2 || a[0] != '-') break;
    if (a == "--") { i++; break; }
    // A leading -N (digits) is a command spec, not options.
    if (std::isdigit(static_cast<unsigned char>(a[1]))) break;
    bool consumed_next = false;
    for (size_t k = 1; k < a.size(); k++) {
      switch (a[k]) {
        case 'l': list = true; break;
        case 'n': nonum = true; break;
        case 'r': reverse = true; break;
        case 's': subst = true; break;
        case 'e':
          have_e = true;
          if (k + 1 < a.size()) { ename = a.substr(k + 1); k = a.size(); }
          else if (i + 1 < argv.size()) { ename = argv[i + 1]; consumed_next = true; }
          break;
        default:
          std::fprintf(stderr, "%sfc: -%c: invalid option\n", sh.err_prefix().c_str(),
                       a[k]);
          std::fprintf(stderr, "%s\n", kUsage);
          return 2;
      }
    }
    if (consumed_next) i++;
  }
  // `fc -e -' is the re-execute-without-editing form: same as `fc -s'.
  if (have_e && ename == "-") subst = true;

  HIST_ENTRY **hl = history_list();
  int n = 0;
  if (hl) while (hl[n]) n++;
  if (n == 0) return 0;

  // The fc invocation's own entry (when it was recorded) is skipped: fc works
  // on the commands before it (bash's hist_last_line_added).
  bool self_recorded = sh.hist_cur_cmd_index >= 0 && sh.hist_cur_cmd_index == n - 1;
  int last_hist = n - 1 - (self_recorded ? 1 : 0);
  int real_last = n - 1;
  if (last_hist < 0) last_hist = 0;

  auto no_command = [&]() {
    std::fprintf(stderr, "%sfc: no command found\n", sh.err_prefix().c_str());
    return 1;
  };
  auto out_of_range = [&]() {
    std::fprintf(stderr, "%sfc: history specification out of range\n",
                 sh.err_prefix().c_str());
    return 1;
  };

  // bash's fc_gethnum: resolve SPEC to a 0-based index.  Numeric specs clamp
  // to the valid range (hn_first selects which end); `-0' is special.
  auto gethnum = [&](const std::string &spec, bool hn_first) -> int {
    const char *s = spec.c_str();
    int sign = 1;
    if (*s == '-') { sign = -1; s++; }
    if (std::isdigit(static_cast<unsigned char>(*s))) {
      const char *q = s;
      while (std::isdigit(static_cast<unsigned char>(*q))) q++;
      long v = std::atol(s) * sign;
      (void)q;
      if (v < 0) {
        v += last_hist + 1;
        return v < 0 ? 0 : static_cast<int>(v);
      }
      if (v == 0) return sign == -1 ? (list ? real_last : kInvalid) : last_hist;
      v -= history_base;
      if (v < 0) return hn_first ? 0 : last_hist;
      if (v > last_hist) return hn_first ? 0 : last_hist;
      return static_cast<int>(v);
    }
    for (int k = last_hist; k >= 0; k--)
      if (std::strncmp(hl[k]->line, spec.c_str(), spec.size()) == 0) return k;
    return kNotFound;
  };

  // Replace fc's own entry with the command it ran (bash's fc_replhist), or
  // record the command when fc itself wasn't recorded.
  auto replace_self = [&](const std::string &cmd) {
    if (self_recorded) {
      HIST_ENTRY *old = replace_history_entry(n - 1, cmd.c_str(), nullptr);
      if (old) free_history_entry(old);
    } else {
      add_history(cmd.c_str());
      sh.hist_new_entries++;
    }
  };

  if (subst) {  // fc -s [pat=rep ...] [command]: re-run with substitutions
    std::vector<std::pair<std::string, std::string>> subs;
    std::string spec;
    for (; i < argv.size(); i++) {
      auto eq = argv[i].find('=');
      if (eq != std::string::npos && spec.empty())
        subs.emplace_back(argv[i].substr(0, eq), argv[i].substr(eq + 1));
      else
        spec = argv[i];
    }
    int idx = spec.empty() ? last_hist : gethnum(spec, false);
    if (idx < 0) return no_command();
    std::string cmd = hl[idx]->line;
    for (const auto &pr : subs) {          // each pat=rep applies globally
      if (pr.first.empty()) continue;
      size_t pos = 0;
      while ((pos = cmd.find(pr.first, pos)) != std::string::npos) {
        cmd.replace(pos, pr.first.size(), pr.second);
        pos += pr.second.size();
      }
    }
    std::fprintf(stderr, "%s\n", cmd.c_str());
    replace_self(cmd);
    return sh.run_string(cmd);
  }

  std::vector<std::string> specs;
  for (; i < argv.size(); i++) specs.push_back(argv[i]);

  int histbeg, histend;
  if (!specs.empty()) {
    histbeg = gethnum(specs[0], true);
    if (specs.size() > 1)
      histend = gethnum(specs[1], false);
    else if (histbeg == real_last)
      histend = list ? real_last : histbeg;
    else
      histend = list ? last_hist : histbeg;
  } else if (list) {
    histend = last_hist;
    histbeg = histend - 16 + 1;
    if (histbeg < 0) histbeg = 0;
  } else {
    histbeg = histend = last_hist;
  }
  if (histbeg == kInvalid || histend == kInvalid) return out_of_range();
  if (histbeg == kNotFound || histend == kNotFound) return no_command();
  if (histbeg > histend) { int t = histbeg; histbeg = histend; histend = t; reverse = !list || reverse; }

  if (list) {
    auto emit = [&](int k) {
      if (nonum) std::printf("\t %s\n", hl[k]->line);
      else std::printf("%d\t %s\n", history_base + k, hl[k]->line);
    };
    if (reverse) for (int k = histend; k >= histbeg; k--) emit(k);
    else for (int k = histbeg; k <= histend; k++) emit(k);
    return 0;
  }

  // fc [-e ename] [first] [last]: edit and re-execute.
  std::string text;
  if (reverse) for (int k = histend; k >= histbeg; k--) { text += hl[k]->line; text += '\n'; }
  else for (int k = histbeg; k <= histend; k++) { text += hl[k]->line; text += '\n'; }

  char tmpl[] = "/tmp/gnash_fc_XXXXXX";
  int fd = mkstemp(tmpl);
  if (fd < 0) return 1;
  (void)!write(fd, text.data(), text.size());
  close(fd);

  if (!have_e) {
    ename = sh.get("FCEDIT");
    if (ename.empty()) ename = sh.get("EDITOR");
    if (ename.empty()) ename = "vi";
  }
  // `fc -e -' re-executes without editing.
  if (ename != "-") {
    int est = sh.run_string(ename + " " + tmpl);
    if (est != 0) { unlink(tmpl); return est; }
  }

  std::ifstream f(tmpl);
  std::ostringstream ss;
  ss << f.rdbuf();
  unlink(tmpl);
  std::string cmd = ss.str();
  while (!cmd.empty() && cmd.back() == '\n') cmd.pop_back();
  if (cmd.empty()) return 0;
  std::printf("%s\n", cmd.c_str());
  std::fflush(stdout);
  replace_self(cmd);
  return sh.run_string(cmd);
}

// ---- compgen / complete / compopt ----------------------------------------
static const char *const kReservedWords[] = {
    "if", "then", "else", "elif", "fi", "case", "esac", "for", "select", "while",
    "until", "do", "done", "in", "function", "time", "{", "}", "!", "[[", "]]",
    "coproc", nullptr};

// Files (or directories) whose path begins with WORD, for compgen -f/-d.
static void compgen_files(const std::string &word, bool dirs_only, std::vector<std::string> &out) {
  std::string dir = ".", base = word;
  auto slash = word.rfind('/');
  if (slash != std::string::npos) { dir = word.substr(0, slash + 1); base = word.substr(slash + 1); }
  DIR *d = opendir(dir == "." && word.find('/') == std::string::npos ? "." : (slash == std::string::npos ? "." : word.substr(0, slash + 1)).c_str());
  std::string realdir = (slash == std::string::npos) ? "." : word.substr(0, slash);
  if (realdir.empty()) realdir = "/";
  if (d) closedir(d);
  d = opendir(realdir.c_str());
  if (!d) return;
  std::string pathprefix = (slash == std::string::npos) ? "" : word.substr(0, slash + 1);
  struct dirent *e;
  while ((e = readdir(d)) != nullptr) {
    std::string name = e->d_name;
    if (name == "." || name == "..") continue;
    if (name.compare(0, base.size(), base) != 0) continue;
    std::string full = pathprefix + name;
    if (dirs_only) {
      struct stat st;
      if (stat(full.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) continue;
    }
    out.push_back(full);
  }
  closedir(d);
}

// bash's compopts[]/compacts[] tables, giving `complete -p' its canonical order
// and naming compgen/complete's `-o' options and `-A' actions.
static const char *const kCompOpts[] = {
    "bashdefault", "default", "dirnames", "filenames", "fullquote",
    "noquote", "nosort", "nospace", "plusdirs", nullptr};
struct CompActEnt { const char *name; char opt; };
static const CompActEnt kCompActs[] = {
    {"alias", 'a'},   {"arrayvar", 0},  {"binding", 0},  {"builtin", 'b'},
    {"command", 'c'}, {"directory", 'd'}, {"disabled", 0}, {"enabled", 0},
    {"export", 'e'},  {"file", 'f'},    {"function", 0}, {"helptopic", 0},
    {"hostname", 0},  {"group", 'g'},   {"job", 'j'},    {"keyword", 'k'},
    {"running", 0},   {"service", 's'}, {"setopt", 0},   {"shopt", 0},
    {"signal", 0},    {"stopped", 0},   {"user", 'u'},   {"variable", 'v'},
    {nullptr, 0}};

// bash's sh_single_quote: wrap in single quotes, embedded ' becomes '\''.
static std::string comp_squote(const std::string &s) {
  std::string r = "'";
  for (char c : s) { if (c == '\'') r += "'\\''"; else r += c; }
  return r + "'";
}

// FNV variant matching bash's hash_string / gnash's assoc_hash_string, so the
// compspec listing walks the same 512-bucket table order bash does.
static unsigned comp_hash(const std::string &s) {
  unsigned i = 2166136261u;
  for (unsigned char c : s) { i += (i << 1) + (i << 4) + (i << 7) + (i << 8) + (i << 24); i ^= c; }
  return i;
}

// The pseudo-topics `help' knows about beyond the builtins, for `-A helptopic'.
static const char *const kHelpSpecials[] = {
    "!", "%", "(( ... ))", "[[ ... ]]", "{ ... }", "case", "coproc", "for",
    "for ((", "function", "if", "select", "time", "until", "variables",
    "while", nullptr};

// Collect the raw candidate list for compgen's actions (before prefix filter).
static void compgen_collect(Shell &sh, const std::vector<std::string> &actions,
                            const std::string &word, std::vector<std::string> &c) {
  for (const std::string &act : actions) {
    if (act == "builtin") { for (const auto &n : builtin_names_sorted()) c.push_back(n); }
    else if (act == "enabled" || act == "disabled") {
      // gnash has no disabled builtins; `enabled' is every builtin.
      if (act == "enabled") for (const auto &n : builtin_names_sorted()) c.push_back(n);
    }
    else if (act == "helptopic") {
      std::vector<std::string> t = builtin_names_sorted();
      for (const char *const *s = kHelpSpecials; *s; s++) t.push_back(*s);
      std::sort(t.begin(), t.end());
      for (const auto &n : t) c.push_back(n);
    }
    else if (act == "setopt") { for (const auto &o : set_option_states(sh)) c.push_back(o.first); }
    else if (act == "shopt") { for (const auto &kv : sh.shopt_opts) c.push_back(kv.first); }
    else if (act == "keyword") { for (int i = 0; kReservedWords[i]; i++) c.push_back(kReservedWords[i]); }
    else if (act == "variable") { for (const auto &kv : sh.vars) c.push_back(kv.first); }
    else if (act == "export") { for (const auto &kv : sh.vars) if (kv.second.exported) c.push_back(kv.first); }
    else if (act == "alias") { for (const auto &kv : sh.aliases) c.push_back(kv.first); }
    else if (act == "function") { for (const auto &kv : sh.functions) c.push_back(kv.first); }
    else if (act == "file") { compgen_files(word, false, c); }
    else if (act == "directory") { compgen_files(word, true, c); }
    else if (act == "command") {  // command names: keywords, builtins, functions, aliases, PATH
        for (int i = 0; kReservedWords[i]; i++) c.push_back(kReservedWords[i]);
        for (const auto &n : builtin_names_sorted()) c.push_back(n);
        for (const auto &kv : sh.functions) c.push_back(kv.first);
        for (const auto &kv : sh.aliases) c.push_back(kv.first);
        std::string path = sh.get("PATH");
        size_t p = 0;
        while (p <= path.size()) {
          size_t q = path.find(':', p);
          std::string dir = path.substr(p, q == std::string::npos ? std::string::npos : q - p);
          if (dir.empty()) dir = ".";
          if (DIR *dp = opendir(dir.c_str())) {
            struct dirent *e;
            while ((e = readdir(dp)) != nullptr) {
              std::string nm = e->d_name;
              if (!word.empty() && nm.compare(0, word.size(), word) != 0) continue;
              if (access((dir + "/" + nm).c_str(), X_OK) == 0) c.push_back(nm);
            }
            closedir(dp);
          }
          if (q == std::string::npos) break;
          p = q + 1;
        }
    }
    // Unhandled actions (arrayvar/binding/hostname/job/signal/...) yield nothing.
  }
}

// Map a compgen short action flag to its action name (subset of kCompActs).
static const char *compgen_short_action(char o) {
  for (const CompActEnt *e = kCompActs; e->name; e++)
    if (e->opt == o) return e->name;
  return nullptr;
}
static bool compgen_valid_action(const std::string &a) {
  for (const CompActEnt *e = kCompActs; e->name; e++)
    if (a == e->name) return true;
  return false;
}
static bool compgen_valid_option(const std::string &o) {
  for (const char *const *p = kCompOpts; *p; p++) if (o == *p) return true;
  return false;
}
static bool valid_identifier(const std::string &s) {
  if (s.empty() || (!std::isalpha((unsigned char)s[0]) && s[0] != '_')) return false;
  for (char c : s) if (!std::isalnum((unsigned char)c) && c != '_') return false;
  return true;
}

int bi_compgen(Shell &sh, const std::vector<std::string> &argv) {
  static const char *const kUsage =
      "compgen: usage: compgen [-V varname] [-abcdefgjksuv] [-o option] "
      "[-A action] [-G globpat] [-W wordlist] [-F function] [-C command] "
      "[-X filterpat] [-P prefix] [-S suffix] [word]\n";
  std::vector<std::string> words;
  std::vector<std::string> actions;
  std::string prefix, suffix, word, filterpat, varname;
  bool has_filter = false, has_var = false;
  size_t i = 1;
  auto optarg = [&](const std::string &a) -> std::string {
    if (a.size() > 2) return a.substr(2);
    return (i + 1 < argv.size()) ? argv[++i] : std::string();
  };
  for (; i < argv.size(); i++) {
    const std::string &a = argv[i];
    if (a == "--") { i++; break; }
    if (a.size() < 2 || a[0] != '-') break;
    char o = a[1];
    if (o == 'W') { std::istringstream iss(optarg(a)); std::string w; while (iss >> w) words.push_back(w); }
    else if (o == 'P') prefix = optarg(a);
    else if (o == 'S') suffix = optarg(a);
    else if (o == 'X') { filterpat = optarg(a); has_filter = true; }
    else if (o == 'V') { varname = optarg(a); has_var = true; }
    else if (o == 'A') {
      std::string act = optarg(a);
      if (!compgen_valid_action(act)) {
        std::fflush(stdout);
        std::fprintf(stderr, "%scompgen: %s: invalid action name\n", sh.err_prefix().c_str(), act.c_str());
        return 1;
      }
      actions.push_back(act);
    } else if (o == 'o') {
      std::string opt = optarg(a);
      if (!compgen_valid_option(opt)) {
        std::fflush(stdout);
        std::fprintf(stderr, "%scompgen: %s: invalid option name\n", sh.err_prefix().c_str(), opt.c_str());
        return 1;
      }
    } else if (o == 'G' || o == 'F' || o == 'C') {
      optarg(a);  // consume the argument of an unsupported generator
    } else if (compgen_short_action(o)) {
      for (size_t j = 1; j < a.size(); j++)
        if (const char *ac = compgen_short_action(a[j])) actions.push_back(ac);
    } else {
      std::fflush(stdout);
      std::fprintf(stderr, "%scompgen: -%c: invalid option\n", sh.err_prefix().c_str(), o);
      std::fprintf(stderr, "%s", kUsage);
      return 2;
    }
  }
  if (has_var && !valid_identifier(varname)) {
    std::fflush(stdout);
    std::fprintf(stderr, "%scompgen: `%s': not a valid identifier\n", sh.err_prefix().c_str(), varname.c_str());
    return 1;
  }
  if (i < argv.size()) word = argv[i];

  std::vector<std::string> cands = words;
  compgen_collect(sh, actions, word, cands);

  // -X filterpat removes candidates that match; a leading `!' inverts (keeps
  // only matches).  The prefix/suffix are applied after filtering.
  bool invert = has_filter && !filterpat.empty() && filterpat[0] == '!';
  std::string fpat = invert ? filterpat.substr(1) : filterpat;
  std::vector<std::string> results;
  for (const std::string &c : cands) {
    if (!word.empty() && c.compare(0, word.size(), word) != 0) continue;
    if (has_filter && !fpat.empty()) {
      std::string p = fpat, t = c;
      bool m = strmatch(p.data(), t.data(), FNM_EXTMATCH) == 0;
      if (m != invert) continue;  // drop matches (or non-matches when inverted)
    }
    results.push_back(prefix + c + suffix);
  }
  if (has_var) {  // store the results in the named array instead of printing
    std::vector<std::pair<std::optional<std::string>, std::string>> elems;
    for (const auto &r : results) elems.push_back({std::nullopt, r});
    sh.array_assign(varname, elems, false, false);
    return results.empty() ? 1 : 0;
  }
  for (const std::string &r : results) std::printf("%s\n", r.c_str());
  return results.empty() ? 1 : 0;
}

// bash's `complete -p' reconstruction (builtins/complete.def:print_one_completion):
// options and actions are emitted in a fixed canonical order, not the order the
// user gave them.  These tables mirror bash's compopts[]/compacts[].
// Reconstruct a `complete' spec string in bash's canonical order for -p output.
int bi_complete(Shell &sh, const std::vector<std::string> &argv) {
  static const char *const kUsage =
      "complete: usage: complete [-abcdefgjksuv] [-pr] [-DEI] [-o option] "
      "[-A action] [-G globpat] [-W wordlist] [-F function] [-C command] "
      "[-X filterpat] [-P prefix] [-S suffix] [name ...]\n";
  bool print = false, remove = false;
  std::set<std::string> o_set, act_set;
  std::string g_arg, w_arg, p_arg, s_arg, x_arg, c_arg, f_arg;
  bool has_g = false, has_w = false, has_p = false, has_s = false,
       has_x = false, has_c = false, has_f = false;
  std::vector<std::string> names;
  auto short_action = [](char o) -> const char * {
    for (const CompActEnt *e = kCompActs; e->name; e++)
      if (e->opt == o) return e->name;
    return nullptr;
  };
  for (size_t i = 1; i < argv.size(); i++) {
    const std::string &a = argv[i];
    if (a == "-p") { print = true; continue; }
    if (a == "-r") { remove = true; continue; }
    if (a.size() >= 2 && a[0] == '-' && a != "--") {
      auto takes = [&](std::string &dst, bool &has) {
        has = true;
        if (a.size() > 2) dst = a.substr(2);
        else if (i + 1 < argv.size()) dst = argv[++i];
      };
      char o = a[1];
      switch (o) {
        case 'o': { std::string v; bool h; takes(v, h); o_set.insert(v); break; }
        case 'A': { std::string v; bool h; takes(v, h); act_set.insert(v); break; }
        case 'G': takes(g_arg, has_g); break;
        case 'W': takes(w_arg, has_w); break;
        case 'P': takes(p_arg, has_p); break;
        case 'S': takes(s_arg, has_s); break;
        case 'X': takes(x_arg, has_x); break;
        case 'C': takes(c_arg, has_c); break;
        case 'F': takes(f_arg, has_f); break;
        default:
          if (const char *act = short_action(o)) act_set.insert(act);
          else {
            std::fflush(stdout);
            std::fprintf(stderr, "%scomplete: -%c: invalid option\n",
                         sh.err_prefix().c_str(), o);
            std::fprintf(stderr, "%s", kUsage);
            return 2;
          }
      }
      continue;
    }
    if (a == "--") continue;
    names.push_back(a);
  }

  if (remove) {
    if (names.empty()) { sh.completions.clear(); sh.completion_order.clear(); }
    else
      for (const auto &n : names) {
        if (sh.completions.erase(n)) {
          auto &ord = sh.completion_order;
          ord.erase(std::remove(ord.begin(), ord.end(), n), ord.end());
        } else {
          std::fflush(stdout);
          std::fprintf(stderr, "%scomplete: %s: no completion specification\n",
                       sh.err_prefix().c_str(), n.c_str());
          return 1;
        }
      }
    return 0;
  }

  // Build the canonical spec text (empty for a bare `complete name').
  std::string spec;
  auto add = [&](const std::string &s) { if (!spec.empty()) spec += ' '; spec += s; };
  for (const char *const *o = kCompOpts; *o; o++)
    if (o_set.count(*o)) add(std::string("-o ") + *o);
  for (const CompActEnt *e = kCompActs; e->name; e++)  // short flags first
    if (e->opt && act_set.count(e->name)) add(std::string("-") + e->opt);
  for (const CompActEnt *e = kCompActs; e->name; e++)  // then long-only -A actions
    if (e->opt == 0 && act_set.count(e->name)) add(std::string("-A ") + e->name);
  if (has_g) add("-G " + comp_squote(g_arg));
  if (has_w) add("-W " + comp_squote(w_arg));
  if (has_p) add("-P " + comp_squote(p_arg));
  if (has_s) add("-S " + comp_squote(s_arg));
  if (has_x) add("-X " + comp_squote(x_arg));
  if (has_c) add("-C " + comp_squote(c_arg));
  if (has_f) add("-F " + (sub_has_metas(f_arg) ? comp_squote(f_arg) : f_arg));

  auto emit = [&](const std::string &name, const std::string &sp) {
    if (sp.empty()) std::printf("complete %s\n", name.c_str());
    else std::printf("complete %s %s\n", sp.c_str(), name.c_str());
  };
  // List all compspecs in bash's hash-table walk order: bucket asc (bash's
  // prog_completes has COMPLETE_HASH_BUCKETS=512), newest-first within a bucket.
  auto walk_order = [&]() {
    std::vector<std::string> ord;
    struct E { unsigned bucket; size_t seq; const std::string *name; };
    std::vector<E> es;
    for (size_t k = 0; k < sh.completion_order.size(); k++) {
      const std::string &n = sh.completion_order[k];
      if (sh.completions.count(n)) es.push_back({comp_hash(n) & 511u, k, &n});
    }
    std::stable_sort(es.begin(), es.end(), [](const E &a, const E &b) {
      if (a.bucket != b.bucket) return a.bucket < b.bucket;
      return a.seq > b.seq;
    });
    for (const auto &e : es) ord.push_back(*e.name);
    return ord;
  };

  // Options but no name (and not -p/-r) is a usage error; a bare `complete'
  // with nothing at all lists every compspec.
  if (!print && names.empty() && !spec.empty()) {
    std::fflush(stdout);
    std::fprintf(stderr, "%s", kUsage);
    return 2;
  }
  if (print || names.empty()) {
    if (names.empty()) {
      for (const auto &n : walk_order()) emit(n, sh.completions[n]);
      return 0;
    }
    int st = 0;
    for (const auto &n : names) {
      auto it = sh.completions.find(n);
      if (it != sh.completions.end()) emit(n, it->second);
      else {
        std::fflush(stdout);
        std::fprintf(stderr, "%scomplete: %s: no completion specification\n",
                     sh.err_prefix().c_str(), n.c_str());
        st = 1;
      }
    }
    return st;
  }
  for (const auto &n : names) {
    if (!sh.completions.count(n)) sh.completion_order.push_back(n);
    sh.completions[n] = spec;
  }
  return 0;
}

int bi_compopt(Shell &sh, const std::vector<std::string> &argv) {
  // bash validates -o/+o option names before reporting that no completion is in
  // progress; an unknown name errors first.
  for (size_t i = 1; i < argv.size(); i++) {
    const std::string &a = argv[i];
    if ((a == "-o" || a == "+o") && i + 1 < argv.size()) {
      const std::string &opt = argv[++i];
      if (!compgen_valid_option(opt)) {
        std::fprintf(stderr, "%scompopt: %s: invalid option name\n",
                     sh.err_prefix().c_str(), opt.c_str());
        return 1;
      }
    } else if (a == "-D" || a == "-E" || a == "-I") {
      continue;
    } else if (!a.empty() && (a[0] == '-' || a[0] == '+') && a != "--") {
      std::fprintf(stderr, "%scompopt: %c%c: invalid option\n",
                   sh.err_prefix().c_str(), a[0], a.size() > 1 ? a[1] : ' ');
      return 2;
    }
  }
  std::fprintf(stderr, "%scompopt: not currently executing completion function\n",
               sh.err_prefix().c_str());
  return 1;  // gnash never runs a completion function, so this always applies
}

// ---- bind ----------------------------------------------------------------
int bi_bind(Shell &sh, const std::vector<std::string> &argv) {
  if (!sh.interactive)
    std::fprintf(stderr, "%sbind: warning: line editing not enabled\n", sh.err_prefix().c_str());
  for (size_t i = 1; i < argv.size(); i++) {
    const std::string &a = argv[i];
    if (a == "-l") {
      for (const char **n = rl_funmap_names(); *n; n++) std::printf("%s\n", *n);
    } else if (a == "-f" || a == "-x") {
      if (i + 1 < argv.size()) {
        if (a == "-f") rl_read_init_file(argv[++i].c_str());
        else i++;  // -x 'keyseq:command' -- accepted, not wired to execution
      }
    } else if (a == "-m" || a == "-q" || a == "-u" || a == "-r" || a == "-p" ||
               a == "-P" || a == "-v" || a == "-V" || a == "-s" || a == "-S") {
      if ((a == "-m" || a == "-q" || a == "-u" || a == "-r") && i + 1 < argv.size()) i++;
      // listing forms (-p/-v/...) produce no output in this subset
    } else if (!a.empty() && a[0] != '-') {
      std::string line = a;  // "keyseq: function"
      rl_parse_and_bind(const_cast<char *>(line.c_str()));
    }
  }
  (void)sh;
  return 0;
}

}  // namespace

// [[ ]] evaluation over the reconstructed expression (re-tokenized).
bool eval_cond_expression(Shell &sh, const std::string &expr, int *status);

// Would NAME run as a command?  Used by syntax highlighting.
bool command_is_valid(Shell &sh, const std::string &name) {
  if (name.empty()) return false;
  if (name.find('/') != std::string::npos) return access(name.c_str(), X_OK) == 0;
  if (is_builtin_name(name) || is_reserved_word(name)) return true;
  if (sh.functions.count(name) || sh.aliases.count(name)) return true;
  return !find_in_path(sh, name).empty();
}

std::vector<std::string> command_completions(Shell &sh, const std::string &prefix) {
  std::set<std::string> out;
  auto consider = [&](const std::string &n) {
    if (n.size() >= prefix.size() && n.compare(0, prefix.size(), prefix) == 0) out.insert(n);
  };
  // Shell keywords, aliases, functions, and builtins (skipping any `enable -n'd).
  static const char *kw[] = {"if",   "then",     "else",   "elif", "fi",     "for",
                             "while", "until",   "do",     "done", "case",   "esac",
                             "in",   "function", "select", "time", "coproc", nullptr};
  for (int i = 0; kw[i]; i++) consider(kw[i]);
  for (const auto &kv : sh.aliases) consider(kv.first);
  for (const auto &kv : sh.functions) consider(kv.first);
  for (const auto &b : builtin_names_sorted())
    if (!sh.disabled_builtins.count(b)) consider(b);
  // Executable regular files on $PATH, matched by basename.
  std::string path = sh.get("PATH");
  size_t i = 0;
  while (i <= path.size()) {
    size_t j = path.find(':', i);
    std::string dir = path.substr(i, j == std::string::npos ? std::string::npos : j - i);
    if (dir.empty()) dir = ".";
    if (DIR *d = opendir(dir.c_str())) {
      while (struct dirent *e = readdir(d)) {
        const char *nm = e->d_name;
        if (std::strncmp(nm, prefix.c_str(), prefix.size()) != 0) continue;
        std::string full = dir + "/" + nm;
        struct stat st;
        if (access(full.c_str(), X_OK) == 0 && stat(full.c_str(), &st) == 0 &&
            S_ISREG(st.st_mode))
          out.insert(nm);
      }
      closedir(d);
    }
    if (j == std::string::npos) break;
    i = j + 1;
  }
  return std::vector<std::string>(out.begin(), out.end());
}

// personality [ -lLR ] [ NAME [ -c command ] ]   (alias: emulate)
//
// Switch the shell's personality at runtime, with syntax identical to zsh's
// `emulate' builtin.  With no NAME, print the current personality.  `-c command'
// runs command under NAME then restores the previous personality; `-L' makes
// the switch local to the enclosing function (restored on return); `-R'/`-l'
// are accepted (a personality switch is already a full reconfiguration).
int bi_personality(Shell &sh, const std::vector<std::string> &argv) {
  static const std::set<std::string> kNames = {
      "bash", "zsh",   "sh",    "dash",  "ash",  "ksh",
      "ksh93", "mksh", "pdksh", "rksh",  "csh",  "tcsh"};
  bool opt_l = false, opt_L = false, opt_R = false;
  size_t i = 1;
  for (; i < argv.size(); i++) {
    const std::string &a = argv[i];
    if (a == "--") { i++; break; }
    if (a.size() < 2 || a[0] != '-') break;
    bool ok = true;
    for (size_t k = 1; k < a.size(); k++) {
      if (a[k] == 'l') opt_l = true;
      else if (a[k] == 'L') opt_L = true;
      else if (a[k] == 'R') opt_R = true;
      else { ok = false; break; }
    }
    if (!ok) {
      std::fprintf(stderr, "%s: bad option: %s\n", argv[0].c_str(), a.c_str());
      return 1;
    }
  }
  (void)opt_R;

  if (i >= argv.size()) {  // no mode -> report the current personality
    std::printf("%s\n", sh.personality_name.c_str());
    return 0;
  }

  std::string mode = argv[i++];
  if (!kNames.count(mode)) {
    std::fprintf(stderr, "%s: unknown personality: %s\n", argv[0].c_str(), mode.c_str());
    return 1;
  }
  // Trailing `-c command' (and any other option-flags, which we accept quietly).
  std::string cmd;
  bool have_c = false;
  for (; i < argv.size(); i++) {
    if (argv[i] == "-c" && i + 1 < argv.size()) { cmd = argv[++i]; have_c = true; }
  }

  if (have_c) {  // run under MODE, then restore
    std::string saved = sh.personality_name;
    sh.set_personality(mode);
    int st = sh.run_string(cmd);
    sh.set_personality(saved);
    return st;
  }
  if (opt_L && !sh.persona_restore.empty() && !sh.persona_restore.back())
    sh.persona_restore.back() = sh.personality_name;  // restore when the function returns
  sh.set_personality(mode);
  (void)opt_l;
  return 0;
}

bool run_builtin(Shell &sh, const std::vector<std::string> &argv, int *status) {
  if (argv.empty()) return false;
  const std::string &cmd = argv[0];
  int st = 0;

  // A builtin disabled with `enable -n' is treated as not a builtin, so an
  // external command of the same name runs instead.
  if (sh.disabled_builtins.count(cmd) && cmd != "enable") return false;

  // In zsh persona, accept common zsh-only builtins as no-ops so zsh startup
  // files (including the system /etc/zshrc) run without "command not found".
  if (sh.persona == Shell::Persona::Zsh) {
    static const std::set<std::string> kZshNoops = {
        "setopt", "unsetopt", "bindkey", "zstyle", "autoload", "zle", "zmodload",
        "compdef", "compinit", "bashcompinit", "disable", "limit", "unlimit",
        "ttyctl", "sched", "zcompile", "add-zsh-hook", "zrecompile"};
    if (kZshNoops.count(cmd)) { if (status) *status = 0; return true; }
  }

  if (cmd == ":" || cmd == "true") st = 0;
  else if (cmd == "false") st = 1;
  else if (cmd == "echo") st = bi_echo(sh, argv);
  else if (cmd == "printf") st = bi_printf(sh, argv);
  else if (cmd == "pwd") st = bi_pwd(sh, argv);
  else if (cmd == "cd") st = bi_cd(sh, argv);
  else if (cmd == "dirs") st = bi_dirs(sh, argv);
  else if (cmd == "pushd") st = bi_pushd(sh, argv);
  else if (cmd == "popd") st = bi_popd(sh, argv);
  else if (cmd == "mapfile" || cmd == "readarray") st = bi_mapfile(sh, argv);
  else if (cmd == "export") st = bi_export(sh, argv);
  else if (cmd == "unset") st = bi_unset(sh, argv);
  else if (cmd == "set") st = bi_set(sh, argv);
  // `personality' is always available; `emulate' is its zsh-mode alias.
  else if (cmd == "personality" || (cmd == "emulate" && sh.is_zsh()))
    st = bi_personality(sh, argv);
  else if (cmd == "read") st = bi_read(sh, argv);
  else if (cmd == "test") st = bi_test(sh, argv, false);
  else if (cmd == "[") st = bi_test(sh, argv, true);
  else if (cmd == "shift") {
    size_t ai = 1;
    if (ai < argv.size() && argv[ai] == "--") ai++;  // end-of-options marker
    if (ai < argv.size() && argv[ai] == "--help") {
      // Help form: the full per-builtin help text is not yet implemented, so
      // treat it as a no-op success rather than a numeric-argument error.
      st = 0;
    } else if (argv.size() - ai > 1) {
      std::fprintf(stderr, "%sshift: too many arguments\n", sh.err_prefix().c_str());
      st = 1;
    } else {
      long n = 1;
      std::string a;
      bool numeric = true;
      if (ai < argv.size()) {
        a = argv[ai];
        char *end = nullptr;
        n = std::strtol(a.c_str(), &end, 10);
        numeric = !a.empty() && end != a.c_str() && *end == '\0';
      }
      if (!numeric) {
        std::fprintf(stderr, "%sshift: %s: numeric argument required\n",
                     sh.err_prefix().c_str(), a.c_str());
        st = 2;
      } else if (n == 0) {
        st = 0;
      } else if (n < 0) {
        // A negative count is always out of range (reported regardless of the
        // shift_verbose option), status 1.
        std::fprintf(stderr, "%sshift: %s: shift count out of range\n",
                     sh.err_prefix().c_str(), a.c_str());
        st = 1;
      } else if (n > static_cast<long>(sh.positional.size())) {
        // Too large: silent failure unless `shopt -s shift_verbose'.
        if (sh.shopt_opts["shift_verbose"])
          std::fprintf(stderr, "%sshift: %s: shift count out of range\n",
                       sh.err_prefix().c_str(), a.c_str());
        st = 1;
      } else {
        for (long k = 0; k < n; k++) sh.positional.erase(sh.positional.begin());
        st = 0;
      }
    }
  } else if (cmd == "exit") {
    sh.exiting = true;
    sh.exit_status = argv.size() > 1 ? (std::atoi(argv[1].c_str()) & 0xff) : sh.last_status;
    st = sh.exit_status;
  } else if (cmd == "return") {
    // `return' is only meaningful in a function or a sourced script; elsewhere
    // it is an error and must not unwind the current input (bash).
    if (!sh.in_function() && sh.source_depth == 0) {
      std::fprintf(stderr, "%sreturn: can only `return' from a function or sourced script\n",
                   sh.err_prefix().c_str());
      st = 1;
    } else {
      sh.returning = true;
      sh.exit_status = argv.size() > 1 ? (std::atoi(argv[1].c_str()) & 0xff) : sh.last_status;
      st = sh.exit_status;
    }
  } else if (cmd == "break" || cmd == "continue") {
    // Outside any loop bash prints a diagnostic (suppressed under `set -o
    // posix') and succeeds without unwinding, rather than silently swallowing
    // the rest of the input.  Mirrors builtins/break.def check_loop_level().
    if (sh.loop_depth == 0) {
      if (!sh.opt_posix)
        std::fprintf(stderr, "%s%s: only meaningful in a `for', `while', or `until' loop\n",
                     sh.err_prefix().c_str(), cmd.c_str());
      st = 0;
    } else {
      int n = 1;
      // Skip a leading `--' end-of-options marker (`break -- 5').
      size_t ci = 1;
      if (ci < argv.size() && argv[ci] == "--") ci++;
      const std::string a = ci < argv.size() ? argv[ci] : std::string();
      char *end = nullptr;
      long v = a.empty() ? 1 : std::strtol(a.c_str(), &end, 10);
      if (!a.empty() && (end == a.c_str() || *end != '\0')) {
        // A non-numeric count: bash aborts the command (get_numeric_arg); we
        // report and terminate every enclosing loop, the closest observable
        // approximation without the top-level throw.
        std::fprintf(stderr, "%s%s: %s: numeric argument required\n",
                     sh.err_prefix().c_str(), cmd.c_str(), a.c_str());
        sh.break_count = sh.loop_depth;
        st = 1;
      } else if (v <= 0) {
        // A non-positive count breaks out of every enclosing loop (bash sets
        // `breaking = loop_level' for both break and continue), status 1.
        std::fprintf(stderr, "%s%s: %s: loop count out of range\n",
                     sh.err_prefix().c_str(), cmd.c_str(), a.c_str());
        sh.break_count = sh.loop_depth;
        st = 1;
      } else {
        // Requesting more levels than are active caps at the outermost loop, so
        // the remaining count never unwinds past the loop nest.
        n = static_cast<int>(v);
        if (n > sh.loop_depth) n = sh.loop_depth;
        if (cmd == "break") sh.break_count = n;
        else sh.continue_count = n;
        st = 0;
      }
    }
  } else if (cmd == "eval") {
    // `eval' takes no options besides `--'; a leading `-X' is an error.
    size_t ai = 1;
    if (ai < argv.size() && argv[ai] != "--" && argv[ai].size() >= 2 && argv[ai][0] == '-') {
      std::fprintf(stderr, "%seval: %s: invalid option\n", sh.err_prefix().c_str(),
                   argv[ai].c_str());
      std::fprintf(stderr, "eval: usage: eval [arg ...]\n");
      st = 2;
    } else {
      if (ai < argv.size() && argv[ai] == "--") ai++;
      std::string saved_ctx = sh.error_context;
      sh.error_context = "eval";  // parse errors report `NAME: eval: line N: ...'
      // $LINENO inside eval continues from the eval command's line (bash), so an
      // eval on line 42 whose body is `echo $LINENO' prints 42.
      int saved_base = sh.lineno_base;
      if (sh.cur_lineno > 0) sh.lineno_base = sh.cur_lineno - 1;
      st = sh.run_string(join(argv, ai));
      sh.lineno_base = saved_base;
      sh.error_context = saved_ctx;
    }
  } else if (cmd == "source" || cmd == ".") {
    // `-p PATH' gives an explicit colon-separated search path (bash 5.2+).
    size_t ai = 1;
    bool pflag = false;
    std::string ppath;
    for (; ai < argv.size(); ai++) {
      if (argv[ai] == "--") { ai++; break; }
      if (argv[ai] == "-p") { pflag = true; if (ai + 1 < argv.size()) ppath = argv[++ai]; continue; }
      break;
    }
    if (ai >= argv.size()) {
      std::fprintf(stderr, "%s%s: filename argument required\n", sh.err_prefix().c_str(),
                   cmd.c_str());
      std::fprintf(stderr, "%s: usage: %s [-p path] filename [arguments]\n",
                   cmd.c_str(), cmd.c_str());
      return 2;
    }
    const std::string &fname = argv[ai];
    if (sh.opt_restricted && fname.find('/') != std::string::npos) {
      std::fprintf(stderr, "%s%s: %s: restricted\n", sh.err_prefix().c_str(),
                   cmd.c_str(), fname.c_str());
      return 1;
    }
    // Resolve the file: a name with a slash is used as-is; otherwise search the
    // `-p' path, or $PATH when the `sourcepath' shopt is on, falling back to the
    // current directory.
    std::string path = fname;
    auto search = [&](const std::string &plist) -> bool {
      size_t start = 0;
      while (start <= plist.size()) {
        size_t e = plist.find(':', start);
        std::string dir = plist.substr(start, e == std::string::npos ? std::string::npos : e - start);
        if (dir.empty()) dir = ".";
        std::string cand = dir + "/" + fname;
        if (access(cand.c_str(), R_OK) == 0) { path = cand; return true; }
        if (e == std::string::npos) break;
        start = e + 1;
      }
      return false;
    };
    bool giveup = false;
    if (fname.find('/') != std::string::npos) {
      // used directly
    } else if (pflag) {
      // `-p' searches only the given path; a miss is a hard failure.
      if (!search(ppath)) {
        std::fprintf(stderr, "%s%s: %s: file not found\n", sh.err_prefix().c_str(),
                     cmd.c_str(), fname.c_str());
        st = 1;
        giveup = true;
      }
    } else if (sh.shopt_opts["sourcepath"] && access(fname.c_str(), R_OK) != 0) {
      const char *penv = std::getenv("PATH");
      search(penv ? penv : "");  // on a miss, path stays fname (current directory)
    }
    if (!giveup) {
      std::ifstream f(path);
      if (f) {
        std::ostringstream ss; ss << f.rdbuf();
        // Extra arguments become the sourced file's positional parameters
        // (`. file a b'); with none, it inherits the caller's positionals.
        std::vector<std::string> saved_pos;
        bool set_pos = argv.size() > ai + 1;
        if (set_pos) {
          saved_pos = sh.positional;
          sh.positional.assign(argv.begin() + ai + 1, argv.end());
        }
        // A sourced file becomes the innermost BASH_SOURCE frame; use the
        // resolved path (bash records the PATH-found path, not the bare name),
        // so ${BASH_SOURCE[0]} lets a script locate itself.  The call line is
        // where `source' appears in the current file.
        sh.push_src_frame("source", path, sh.cur_lineno, false);
        // A sourced file has its own line numbering starting at 1 ($LINENO, the
        // DEBUG trap, and functions it defines all use the file's own lines).
        int saved_src_base = sh.lineno_base;
        sh.lineno_base = 0;
        sh.source_depth++;  // `return' is legal while sourcing
        st = sh.run_string(ss.str());
        sh.source_depth--;
        sh.lineno_base = saved_src_base;
        // `return' inside a sourced file ends the file (and sets its status),
        // like reaching EOF -- it must not unwind past `source' or exit the
        // shell (bash semantics; e.g. /etc/bashrc does `[ -z "$PS1" ] && return').
        if (sh.returning) { sh.returning = false; st = sh.exit_status; }
        sh.pop_src_frame();
        if (set_pos) sh.positional = saved_pos;
      } else {
        std::fprintf(stderr, "%s%s: %s\n", sh.err_prefix().c_str(),
                     fname.c_str(), std::strerror(errno));
        st = 1;
      }
    }
  } else if (cmd == "local") {
    if (!sh.in_function()) {
      std::fprintf(stderr, "%slocal: can only be used in a function\n", sh.err_prefix().c_str());
      st = 1;
    } else {
      st = bi_declare(sh, argv, true, false);
    }
  } else if (cmd == "declare" || cmd == "typeset") {
    // Inside a function, declare/typeset without -g makes the variable local.
    st = bi_declare(sh, argv, sh.in_function(), false);
  } else if (cmd == "readonly") {
    st = bi_declare(sh, argv, false, true);
  } else if (cmd == "let") {
    st = bi_let(sh, argv);
  } else if (cmd == "type") {
    st = bi_type(sh, argv);
  } else if (cmd == "help") {
    st = bi_help(sh, argv);
  } else if (cmd == "builtin") {
    st = bi_builtin(sh, argv);
  } else if (cmd == "logout") {
    st = bi_logout(sh, argv);
  } else if (cmd == "hash") {
    st = bi_hash(sh, argv);
  } else if (cmd == "shopt") {
    st = bi_shopt(sh, argv);
  } else if (cmd == "ulimit") {
    st = bi_ulimit(sh, argv);
  } else if (cmd == "enable") {
    st = bi_enable(sh, argv);
  } else if (cmd == "caller") {
    st = bi_caller(sh, argv);
  } else if (cmd == "alias") {
    st = bi_alias(sh, argv);
  } else if (cmd == "unalias") {
    st = bi_unalias(sh, argv);
  } else if (cmd == "history") {
    st = bi_history(sh, argv);
  } else if (cmd == "fc") {
    st = bi_fc(sh, argv);
  } else if (cmd == "compgen") {
    st = bi_compgen(sh, argv);
  } else if (cmd == "complete") {
    st = bi_complete(sh, argv);
  } else if (cmd == "compopt") {
    st = bi_compopt(sh, argv);
  } else if (cmd == "bind") {
    st = bi_bind(sh, argv);
  } else if (cmd == "trap") {
    st = bi_trap(sh, argv);
  } else if (cmd == "umask") {
    st = bi_umask(sh, argv);
  } else if (cmd == "getopts") {
    st = bi_getopts(sh, argv);
  } else if (cmd == "times") {
    st = 0;
  } else if (cmd == "wait") {
    if (argv.size() > 1) {
      Shell::Job *j = sh.job_by_spec(argv[1]);
      if (j) { for (long p : j->pids) st = sh.wait_for_pid(p); }
      else st = sh.wait_for_pid(std::atol(argv[1].c_str()));
    } else {
      st = sh.wait_all();
    }
  } else if (cmd == "jobs") {
    sh.print_jobs();
    st = 0;
  } else if (cmd == "fg") {
    std::string spec = argv.size() > 1 ? argv[1] : "";
    if (!sh.job_control) {
      std::fprintf(stderr, "%sfg: no job control\n", sh.err_prefix().c_str());
      st = 1;
    } else if (Shell::Job *j = sh.job_by_spec(spec)) {
      std::printf("%s\n", j->command.c_str());  // bash echoes the command
      std::fflush(stdout);
      st = sh.foreground_job(*j, true);
    } else {
      std::fprintf(stderr, "%sfg: %s: no such job\n", sh.err_prefix().c_str(),
                   spec.empty() ? "current" : spec.c_str());
      st = 1;
    }
  } else if (cmd == "bg") {
    std::string spec = argv.size() > 1 ? argv[1] : "";
    if (!sh.job_control) {
      std::fprintf(stderr, "%sbg: no job control\n", sh.err_prefix().c_str());
      st = 1;
    } else if (Shell::Job *j = sh.job_by_spec(spec)) {
      sh.background_job(*j, true);
      std::printf("[%d]+ %s &\n", j->id, j->command.c_str());  // bash echoes it
      std::fflush(stdout);
      st = 0;
    } else {
      std::fprintf(stderr, "%sbg: %s: no such job\n", sh.err_prefix().c_str(),
                   spec.empty() ? "current" : spec.c_str());
      st = 1;
    }
  } else if (cmd == "disown") {
    Shell::Job *j = sh.job_by_spec(argv.size() > 1 ? argv[1] : "");
    if (j) {
      int id = j->id;
      sh.jobs.erase(std::remove_if(sh.jobs.begin(), sh.jobs.end(),
                                   [id](const Shell::Job &x) { return x.id == id; }),
                    sh.jobs.end());
    }
    st = 0;
  } else if (cmd == "kill") {
    st = bi_kill(sh, argv);
  } else if (cmd == "suspend") {
    st = 0;
  } else if (cmd == "command") {
    // Execution of `command NAME args' is handled in the executor (run_simple),
    // which runs NAME through the normal builtin/external path with shell
    // functions bypassed.  Only the describe forms (-v/-V) and option errors
    // reach here; the name-lookup helpers this needs live in this file.
    size_t i = 1;
    bool desc_v = false, desc_V = false, bad = false;
    std::string badopt;
    for (; i < argv.size(); i++) {
      const std::string &o = argv[i];
      if (o == "--") { i++; break; }
      if (o.size() < 2 || o[0] != '-') break;
      for (size_t k = 1; k < o.size(); k++) {
        if (o[k] == 'v') desc_v = true;
        else if (o[k] == 'V') desc_V = true;
        else if (o[k] == 'p') {  // default PATH; disallowed under restricted
          if (sh.opt_restricted) {
            std::fprintf(stderr, "%scommand: -p: restricted\n", sh.err_prefix().c_str());
            return 2;
          }
        }
        else { bad = true; badopt = std::string("-") + o[k]; break; }
      }
      if (bad) break;
    }
    if (bad) {
      std::fprintf(stderr, "%scommand: %s: invalid option\n", sh.err_prefix().c_str(),
                   badopt.c_str());
      std::fprintf(stderr, "command: usage: command [-pVv] command [arg ...]\n");
      st = 2;
    } else if (desc_V) {
      // `command -V NAME...' == `type NAME...'.
      std::vector<std::string> t = {"type"};
      t.insert(t.end(), argv.begin() + i, argv.end());
      st = bi_type(sh, t);
    } else if (desc_v) {
      // For each name print the name (function/builtin/keyword) or its full path
      // (external), 0 if all resolved, 1 otherwise.
      st = (i < argv.size()) ? 0 : 1;
      for (size_t j = i; j < argv.size(); j++) {
        const std::string &n = argv[j];
        if (sh.functions.count(n) || is_builtin_name(n) || is_reserved_word(n)) {
          std::printf("%s\n", n.c_str());
        } else {
          std::string p = find_in_path(sh, n);
          if (!p.empty()) std::printf("%s\n", p.c_str());
          else st = 1;
        }
      }
    } else {
      // No -v/-V: execution is handled in run_simple; reached only defensively
      // (e.g. run_builtin called directly).  Run the target if it is a builtin.
      std::vector<std::string> rest(argv.begin() + i, argv.end());
      if (!rest.empty()) run_builtin(sh, rest, &st);
      else st = 0;
    }
  } else if (cmd == "exec") {
    if (sh.opt_restricted) {
      std::fprintf(stderr, "%sexec: restricted\n", sh.err_prefix().c_str());
      return 2;
    }
    // Options: -a NAME (argv[0] for the command), -c (empty environment),
    // -l (login: prefix argv[0] with '-').
    size_t i = 1;
    std::string a_name;
    bool have_a = false, clear_env = false, login = false, bad_opt = false;
    for (; i < argv.size(); i++) {
      const std::string &o = argv[i];
      if (o == "--") { i++; break; }
      if (o.size() < 2 || o[0] != '-') break;
      bool consumed_next = false;
      for (size_t k = 1; k < o.size(); k++) {
        if (o[k] == 'c') clear_env = true;
        else if (o[k] == 'l') login = true;
        else if (o[k] == 'a') {
          have_a = true;
          if (k + 1 < o.size()) { a_name = o.substr(k + 1); }
          else if (i + 1 < argv.size()) { a_name = argv[i + 1]; consumed_next = true; }
          k = o.size();
        } else { bad_opt = true; break; }
      }
      if (bad_opt) break;
      if (consumed_next) i++;
    }
    if (bad_opt) {
      std::fprintf(stderr, "%sexec: %s: invalid option\n", sh.err_prefix().c_str(),
                   argv[i].c_str());
      st = 2;
    } else if (i >= argv.size()) {
      // No command word: `exec' with only options/redirections is a no-op here
      // (redirections are applied by the caller and made permanent).
      st = 0;
    } else {
      const std::string &target = argv[i];
      // Resolve through the shell's $PATH (what `type'/normal execution use), so
      // exec finds exactly what the rest of the shell would run.
      std::string full;
      bool found = false;
      if (target.find('/') != std::string::npos) {
        full = target;
        found = true;  // let execve report ENOENT/EACCES for an explicit path
      } else {
        std::vector<std::string> m = find_all_in_path(sh, target);
        if (!m.empty()) { full = m[0]; found = true; }
        else {
          // No executable match: if a file by that name merely exists on $PATH,
          // use it so execve reports the real error (e.g. EACCES), as bash does.
          std::string path = sh.get("PATH");
          size_t p = 0;
          while (p <= path.size()) {
            size_t q = path.find(':', p);
            std::string dir = path.substr(p, q == std::string::npos ? std::string::npos : q - p);
            if (dir.empty()) dir = ".";
            std::string cand = dir + "/" + target;
            if (access(cand.c_str(), F_OK) == 0) { full = cand; found = true; break; }
            if (q == std::string::npos) break;
            p = q + 1;
          }
        }
      }
      // argv[0] for the command: -a NAME, else the name as typed; -l adds '-'.
      std::string a0 = have_a ? a_name : target;
      if (login) a0 = "-" + a0;
      std::vector<char *> cargv;
      cargv.push_back(const_cast<char *>(a0.c_str()));
      for (size_t j = i + 1; j < argv.size(); j++)
        cargv.push_back(const_cast<char *>(argv[j].c_str()));
      cargv.push_back(nullptr);
      // Pass the shell's current environment (empty under -c) explicitly via
      // execve, so the live shell's `environ' is never disturbed on failure.
      std::vector<std::string> envs;
      if (!clear_env) envs = sh.environ_block();
      std::vector<char *> envp;
      for (auto &e : envs) envp.push_back(const_cast<char *>(e.c_str()));
      envp.push_back(nullptr);

      int code = 127;
      if (found) {
        std::fflush(nullptr);
        execve(full.c_str(), cargv.data(), envp.data());
        // execve returned: it failed.  bash reports this as "<path>: <error>"
        // (no "exec:" prefix), using the resolved path.
        code = (errno == EACCES) ? 126 : 127;
        std::fprintf(stderr, "%s%s: %s\n", sh.err_prefix().c_str(), full.c_str(),
                     std::strerror(errno));
      } else {
        std::fprintf(stderr, "%sexec: %s: not found\n", sh.err_prefix().c_str(), target.c_str());
      }
      // bash: an interactive shell (or `shopt -s execfail') stays alive and
      // returns failure; a non-interactive shell exits.
      auto ef = sh.shopt_opts.find("execfail");
      bool execfail = ef != sh.shopt_opts.end() && ef->second;
      if (sh.interactive || execfail)
        st = code;
      else
        _exit(code);
    }
  } else {
    return false;
  }
  if (status) *status = st;
  return true;
}

// ---- [[ ]] evaluator ------------------------------------------------------
// Re-tokenize the reconstructed expression and evaluate with pattern-matching
// `==`, regex `=~`, string and integer comparisons.

}  // namespace gnash::core

#include "gnash/core/lexer.hpp"

namespace gnash::core {

namespace {

// Compile REG_EXTENDED regexes once and reuse them: regcomp() is expensive
// (it builds a whole automaton) and a loop such as opsh's semver::parse runs
// `[[ $v =~ $re ]]' with the same pattern every iteration, as bash caches too.
regex_t *cached_regex(const std::string &pat) {
  static std::map<std::string, regex_t> cache;
  auto it = cache.find(pat);
  if (it != cache.end()) return &it->second;
  if (cache.size() >= 64) { for (auto &kv : cache) regfree(&kv.second); cache.clear(); }
  regex_t rx;
  if (regcomp(&rx, pat.c_str(), REG_EXTENDED) != 0) return nullptr;
  return &cache.emplace(pat, rx).first->second;  // map owns the compiled data
}

struct CondEval {
  Shell &sh;
  std::vector<Token> t;
  size_t i = 0;
  int synerr = 0;  // forced exit status (2) after a `[[' syntax error, else 0

  bool is_word(const char *w) { return t[i].type == Tok::Word && t[i].text == w; }
  bool at_end() { return t[i].type == Tok::Eof; }

  std::string expand(const std::string &s) {
    Expander ex(sh);
    return ex.expand_no_split(s);
  }

  bool or_expr() {
    bool v = and_expr();
    while (t[i].type == Tok::OrOr) { i++; bool r = and_expr(); v = v || r; }
    return v;
  }
  bool and_expr() {
    bool v = primary();
    while (t[i].type == Tok::AndAnd) { i++; bool r = primary(); v = v && r; }
    return v;
  }
  bool primary() {
    if (t[i].type == Tok::Word && t[i].text == "!") { i++; return !primary(); }
    if (t[i].type == Tok::Lparen) { i++; bool v = or_expr(); if (t[i].type == Tok::Rparen) i++; return v; }
    // unary -X word
    if (t[i].type == Tok::Word && t[i].text.size() == 2 && t[i].text[0] == '-' &&
        std::isalpha(static_cast<unsigned char>(t[i].text[1]))) {
      char o = t[i].text[1];
      i++;
      std::string arg = at_end() ? "" : expand(t[i].text);
      if (!at_end()) i++;
      if (o == 'z') return arg.empty();
      if (o == 'n') return !arg.empty();
      if (o == 'v') {  // -v NAME: true if the variable (or array element) is set
        size_t br = arg.find('[');
        if (br != std::string::npos && !arg.empty() && arg.back() == ']') {
          std::string nm = arg.substr(0, br);
          std::string sub = arg.substr(br + 1, arg.size() - br - 2);
          if (!sh.array_expand_once_ok(nm, sub)) return false;  // diagnostic printed
          if (sub == "@" || sub == "*") return sh.array_count(nm) > 0;
          for (const std::string &k : sh.array_keys(nm)) if (k == sub) return true;
          return false;
        }
        if (sh.is_set(arg)) return true;
        std::string dv;
        if (sh.dynamic_var(arg, dv)) return true;
        if (!arg.empty() && std::isdigit(static_cast<unsigned char>(arg[0]))) {
          size_t idx = static_cast<size_t>(std::atoi(arg.c_str()));
          return idx >= 1 && idx <= sh.positional.size();
        }
        return false;
      }
      if (o == 'R') {  // -R NAME: set and a nameref
        auto it = sh.vars.find(arg);
        return it != sh.vars.end() && it->second.nameref;
      }
      if (o == 't') {  // -t FD: FD must be an integer, else `integer expected'
        char *endp = nullptr;
        long fd = std::strtol(arg.c_str(), &endp, 10);
        if (arg.empty() || *endp != '\0') {
          std::fprintf(stderr, "%s[[: %s: integer expected\n", sh.err_prefix().c_str(),
                       arg.c_str());
          synerr = 2;
          return false;
        }
        return isatty(static_cast<int>(fd)) != 0;
      }
      return file_test(o, arg);
    }
    // word [ binop word ]
    if (t[i].type != Tok::Word) return false;
    std::string lhs = expand(t[i].text);
    i++;
    // binary operator
    if (t[i].type == Tok::Word || t[i].type == Tok::Less || t[i].type == Tok::Great) {
      std::string op = (t[i].type == Tok::Word) ? t[i].text : std::string(tok_name(t[i].type));
      static const char *bops[] = {"==", "=", "!=", "=~", "-eq", "-ne", "-lt",
                                   "-le", "-gt", "-ge", "<", ">", nullptr};
      bool isb = false;
      for (int k = 0; bops[k]; k++) if (op == bops[k]) isb = true;
      if (isb) {
        i++;
        std::string rhs_raw = at_end() ? "" : t[i].text;
        if (!at_end()) i++;
        if (op == "==" || op == "=") {
          std::string pat = expand(rhs_raw);
          std::string p = pat, l = lhs;
          return strmatch(p.data(), l.data(), FNM_EXTMATCH) == 0;
        }
        if (op == "!=") {
          std::string pat = expand(rhs_raw);
          std::string p = pat, l = lhs;
          return strmatch(p.data(), l.data(), FNM_EXTMATCH) != 0;
        }
        if (op == "=~") {
          std::string re = expand(rhs_raw);
          regex_t *rx = cached_regex(re);
          if (!rx) return false;
          size_t ng = rx->re_nsub + 1;
          std::vector<regmatch_t> m(ng);
          bool matched = regexec(rx, lhs.c_str(), ng, m.data(), 0) == 0;
          // BASH_REMATCH[0] is the whole match; [1..] are the capture groups.
          sh.unset("BASH_REMATCH");
          if (matched) {
            for (size_t g = 0; g < ng; g++) {
              std::string sub;
              if (m[g].rm_so >= 0)
                sub = lhs.substr(static_cast<size_t>(m[g].rm_so),
                                 static_cast<size_t>(m[g].rm_eo - m[g].rm_so));
              sh.array_set("BASH_REMATCH", std::to_string(g), sub);
            }
          }
          return matched;
        }
        if (op == "<") return lhs < expand(rhs_raw);
        if (op == ">") return lhs > expand(rhs_raw);
        // The [[ ]] arithmetic comparators evaluate each side as an arithmetic
        // expression (so `-eq 4+3' means 7), reporting a malformed one with
        // bash's `[[: EXPR: ...' diagnostic rather than silently reading 0.
        bool aok = true;
        long l = static_cast<long>(eval_arith_msg(sh, lhs, "[[", &aok));
        long r = static_cast<long>(eval_arith_msg(sh, expand(rhs_raw), "[[", &aok));
        return int_cmp(op, l, r);
      }
    }
    // single argument: non-empty test
    return !lhs.empty();
  }
};

}  // namespace

bool eval_cond_expression(Shell &sh, const std::string &expr, int *status) {
  CondEval ce{sh, tokenize(expr), 0};
  bool v = ce.or_expr();
  if (status) *status = ce.synerr ? ce.synerr : (v ? 0 : 1);
  return v;
}

}  // namespace gnash::core
