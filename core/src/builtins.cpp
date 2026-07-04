// builtins.cpp -- shell builtins, plus the test/[ and [[ ]] evaluators.

#include "gnash/core/builtins.hpp"

#include <algorithm>
#include <cctype>
#include <csignal>
#include <cstdio>
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
#include <sys/stat.h>
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

std::string decode_escapes(const std::string &s) {
  std::string out;
  for (size_t i = 0; i < s.size(); i++) {
    if (s[i] == '\\' && i + 1 < s.size()) {
      switch (s[++i]) {
        case 'n': out += '\n'; break;
        case 't': out += '\t'; break;
        case 'r': out += '\r'; break;
        case 'a': out += '\a'; break;
        case 'b': out += '\b'; break;
        case 'f': out += '\f'; break;
        case 'v': out += '\v'; break;
        case '\\': out += '\\'; break;
        case '0': out += '\0'; break;
        default: out += '\\'; out += s[i]; break;
      }
    } else {
      out += s[i];
    }
  }
  return out;
}

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
  for (size_t j = i; j < argv.size(); j++) {
    if (j > i) out += ' ';
    out += escapes ? decode_escapes(argv[j]) : argv[j];
  }
  std::fwrite(out.data(), 1, out.size(), stdout);
  if (newline) std::fputc('\n', stdout);
  return 0;
}

// ---- %q shell-quoting (matches bash's sh_backslash_quote / ansic_quote) ----
bool q_needs_ansic(const std::string &s) {
  for (unsigned char c : s)
    if (c < 32 || c == 127) return true;
  return false;
}
std::string q_ansic(const std::string &s) {
  std::string r = "$'";
  for (unsigned char c : s) {
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
        if (c < 32 || c == 127) {
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

// Format one numeric/string conversion with width/precision via snprintf.
template <typename T>
void append_formatted(std::string &out, const std::string &spec, T value) {
  int need = std::snprintf(nullptr, 0, spec.c_str(), value);
  if (need < 0) return;
  std::vector<char> buf(static_cast<size_t>(need) + 1);
  std::snprintf(buf.data(), buf.size(), spec.c_str(), value);
  out.append(buf.data(), static_cast<size_t>(need));
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
  auto next = [&]() -> std::string {
    return argi < argv.size() ? argv[argi++] : std::string();
  };
  bool consumed_any = true;
  do {
    consumed_any = false;
    for (size_t i = 0; i < fmt.size(); i++) {
      char c = fmt[i];
      if (c == '\\' && i + 1 < fmt.size()) {
        out += decode_escapes(fmt.substr(i, 2));
        i++;
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
          append_formatted(out, spec, next().c_str());
          consumed_any = true;
        } else if (conv == 'c') {
          std::string a = next();
          if (!a.empty()) out += a[0];
          consumed_any = true;
        } else if (conv == 'd' || conv == 'i' || conv == 'x' || conv == 'X' ||
                   conv == 'o' || conv == 'u') {
          std::string sp = spec;
          sp.insert(sp.size() - 1, "l");  // promote to long
          append_formatted(out, sp, std::strtol(next().c_str(), nullptr, 0));
          consumed_any = true;
        } else if (conv == 'f' || conv == 'F' || conv == 'g' || conv == 'G' ||
                   conv == 'e' || conv == 'E') {
          append_formatted(out, spec, std::strtod(next().c_str(), nullptr));
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

  if (to_var) {
    auto lb = vname.find('[');
    if (lb != std::string::npos && !vname.empty() && vname.back() == ']') {
      sh.array_set(vname.substr(0, lb), vname.substr(lb + 1, vname.size() - lb - 2), out);
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
  const std::vector<std::string> &a;
  size_t i;
  size_t end;
  bool ok = true;

  bool at_end() { return i >= end; }
  const std::string &cur() { return a[i]; }

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

int bi_test(Shell &, const std::vector<std::string> &argv, bool bracket) {
  std::vector<std::string> a(argv.begin() + 1, argv.end());
  if (bracket) {
    if (a.empty() || a.back() != "]") return 2;  // missing ]
    a.pop_back();
  }
  if (a.empty()) return 1;
  TestEval te{a, 0, a.size(), true};
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
int change_dir(Shell &sh, const std::string &dir, bool physical) {
  if (dir.empty()) return 1;
  std::string oldpwd = logical_pwd(sh);
  std::string logical = (dir[0] == '/') ? dir : oldpwd + "/" + dir;
  logical = canon_logical(logical);
  const std::string &target = physical ? dir : logical;
  if (chdir(target.c_str()) != 0) {
    std::fprintf(stderr, "%scd: %s: %s\n", sh.err_prefix().c_str(), dir.c_str(),
                 std::strerror(errno));
    return 1;
  }
  if (!oldpwd.empty()) sh.set_exported("OLDPWD", oldpwd);
  sh.set_exported("PWD", physical ? phys_cwd() : logical);
  return 0;
}

int bi_cd(Shell &sh, const std::vector<std::string> &argv) {
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
std::vector<std::string> full_dirstack(Shell &sh) {
  std::vector<std::string> v;
  v.push_back(logical_pwd(sh));
  for (const std::string &d : sh.dir_stack) v.push_back(d);
  return v;
}

std::string tilde_abbrev(Shell &sh, const std::string &path, bool longform) {
  std::string home = sh.get("HOME");
  if (!longform && !home.empty() &&
      (path == home || (path.size() > home.size() && path.compare(0, home.size(), home) == 0 &&
                        path[home.size()] == '/')))
    return "~" + path.substr(home.size());
  return path;
}

int do_chdir(Shell &sh, const std::string &dir) { return change_dir(sh, dir, false); }

int bi_dirs(Shell &sh, const std::vector<std::string> &argv) {
  bool longform = false, oneline = false, verbose = false;
  for (size_t i = 1; i < argv.size(); i++) {
    const std::string &a = argv[i];
    if (a.empty() || a[0] != '-') break;
    for (size_t k = 1; k < a.size(); k++) {
      if (a[k] == 'c') { sh.dir_stack.clear(); return 0; }
      else if (a[k] == 'l') longform = true;
      else if (a[k] == 'p') oneline = true;
      else if (a[k] == 'v') { oneline = true; verbose = true; }
      else { std::fprintf(stderr, "gnash: dirs: -%c: invalid option\n", a[k]); return 2; }
    }
  }
  std::vector<std::string> v = full_dirstack(sh);
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
  if (argv.size() > 1 && (argv[1][0] == '+' || argv[1][0] == '-') && argv[1].size() > 1 &&
      std::isdigit(static_cast<unsigned char>(argv[1][1]))) {
    // Rotate so the Nth entry becomes the top.
    std::vector<std::string> v = full_dirstack(sh);
    int idx = rot_index(argv[1], v.size());
    if (idx < 0) { std::fprintf(stderr, "gnash: pushd: %s: directory stack index out of range\n", argv[1].c_str()); return 1; }
    std::rotate(v.begin(), v.begin() + idx, v.end());
    if (do_chdir(sh, v[0]) != 0) return 1;
    sh.dir_stack.assign(v.begin() + 1, v.end());
    return bi_dirs(sh, {"dirs"});
  }
  if (argv.size() > 1) {
    std::string old = logical_pwd(sh);
    if (do_chdir(sh, argv[1]) != 0) return 1;
    sh.dir_stack.insert(sh.dir_stack.begin(), old);
    return bi_dirs(sh, {"dirs"});
  }
  // No argument: swap the top two directories.
  if (sh.dir_stack.empty()) {
    std::fprintf(stderr, "gnash: pushd: no other directory\n");
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
    if (idx < 0) { std::fprintf(stderr, "gnash: popd: %s: directory stack index out of range\n", argv[1].c_str()); return 1; }
    if (idx == 0) {  // removing the top: cd to the next entry
      if (sh.dir_stack.empty()) { std::fprintf(stderr, "gnash: popd: directory stack empty\n"); return 1; }
      std::string target = sh.dir_stack.front();
      sh.dir_stack.erase(sh.dir_stack.begin());
      if (do_chdir(sh, target) != 0) return 1;
    } else {
      sh.dir_stack.erase(sh.dir_stack.begin() + (idx - 1));
    }
    return bi_dirs(sh, {"dirs"});
  }
  if (sh.dir_stack.empty()) {
    std::fprintf(stderr, "gnash: popd: directory stack empty\n");
    return 1;
  }
  std::string target = sh.dir_stack.front();
  sh.dir_stack.erase(sh.dir_stack.begin());
  if (do_chdir(sh, target) != 0) return 1;
  return bi_dirs(sh, {"dirs"});
}

// ---- mapfile / readarray -------------------------------------------------

int bi_mapfile(Shell &sh, const std::vector<std::string> &argv) {
  bool strip = false, haveO = false;
  char delim = '\n';
  long count = 0, origin = 0, skip = 0;
  int fd = 0;
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
      case 'u': fd = std::atoi(val.c_str()); break;
      case 'c': case 'C': break;  // callback quantum -- accepted, not invoked
      default: break;
    }
  }
  if (i < argv.size()) name = argv[i];

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
  for (size_t i = 1; i < argv.size(); i++) {
    size_t eq = argv[i].find('=');
    if (eq != std::string::npos)
      sh.set_exported(argv[i].substr(0, eq), argv[i].substr(eq + 1));
    else
      sh.export_name(argv[i]);
  }
  return 0;
}

int bi_unset(Shell &sh, const std::vector<std::string> &argv) {
  bool funcs = false;
  for (size_t i = 1; i < argv.size(); i++) {
    if (argv[i] == "-f") { funcs = true; continue; }
    if (argv[i] == "-v") { funcs = false; continue; }
    if (funcs) sh.functions.erase(argv[i]);
    else sh.unset(argv[i]);
  }
  return 0;
}

// Quote a scalar value for `set' output: bare if it is "simple", else single
// quoted with embedded quotes escaped (matching bash).
std::string set_quote(const std::string &v) {
  bool simple = true;  // an empty value prints bare (name=)
  for (unsigned char c : v)
    if (!(std::isalnum(c) || std::strchr("_-./:=+,%@", c))) { simple = false; break; }
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
    for (const auto &kv : v.assoc) {
      if (!first) s += ' ';
      first = false;
      s += "[" + kv.first + "]=" + set_elem(kv.second);
    }
    std::printf("%s)\n", s.c_str());
  } else {
    std::printf("%s=%s\n", name.c_str(), set_quote(v.value).c_str());
  }
}

// The `set -o'/`set +o' option table with each option's current state.
std::vector<std::pair<std::string, bool>> set_option_states(Shell &sh) {
  bool i = sh.interactive;
  return {
      {"allexport", false},   {"braceexpand", true},
      {"emacs", i},           {"errexit", sh.opt_errexit},
      {"errtrace", sh.opt_functrace}, {"functrace", sh.opt_functrace},
      {"hashall", true},      {"histexpand", i},
      {"history", i},         {"ignoreeof", false},
      {"interactive-comments", true}, {"keyword", false},
      {"monitor", i},         {"noclobber", false},
      {"noexec", false},      {"noglob", sh.opt_noglob},
      {"nolog", false},       {"notify", false},
      {"nounset", sh.opt_nounset}, {"onecmd", false},
      {"physical", false},    {"pipefail", false},
      {"posix", false},       {"privileged", false},
      {"verbose", sh.opt_verbose}, {"vi", false},
      {"xtrace", sh.opt_xtrace}};
}

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
          case 'x': sh.opt_xtrace = on; break;
          case 'u': sh.opt_nounset = on; break;
          case 'f': sh.opt_noglob = on; break;
          case 'v': sh.opt_verbose = on; break;
          case 'T': sh.opt_functrace = on; break;  // DEBUG/RETURN trap inheritance
          case 'o': {
            if (i + 1 >= argv.size()) {
              // `set -o' lists states; `set +o' reproduces as commands.
              for (const auto &o : set_option_states(sh)) {
                if (on) std::printf("%-15s\t%s\n", o.first.c_str(), o.second ? "on" : "off");
                else std::printf("set %co %s\n", o.second ? '-' : '+', o.first.c_str());
              }
            } else {
              std::string o = argv[++i];
              if (o == "errexit") sh.opt_errexit = on;
              else if (o == "xtrace") sh.opt_xtrace = on;
              else if (o == "nounset") sh.opt_nounset = on;
              else if (o == "noglob") sh.opt_noglob = on;
              else if (o == "verbose") sh.opt_verbose = on;
              else if (o == "functrace" || o == "errtrace") sh.opt_functrace = on;
            }
            k = a.size();  // -o consumes the rest of the word
            break;
          }
          default: break;  // other flags accepted as no-ops
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
  std::vector<std::string> names(argv.begin() + 1, argv.end());
  std::string line;
  int c;
  bool got = false;
  while ((c = std::getchar()) != EOF) {
    got = true;
    if (c == '\n') break;
    line += static_cast<char>(c);
  }
  if (!got && line.empty()) return 1;
  if (names.empty()) { sh.set("REPLY", line); return 0; }
  // split by IFS into names; last gets the remainder
  std::string ifs = sh.ifs();
  std::vector<std::string> fields;
  std::string cur;
  auto is_ifs = [&](char ch) { return ifs.find(ch) != std::string::npos; };
  size_t p = 0;
  while (p < line.size() && is_ifs(line[p])) p++;
  for (size_t k = names.size(); k > 1 && p < line.size(); k--) {
    cur.clear();
    while (p < line.size() && !is_ifs(line[p])) cur += line[p++];
    fields.push_back(cur);
    while (p < line.size() && is_ifs(line[p])) p++;
  }
  std::string rest = line.substr(p);
  while (!rest.empty() && is_ifs(rest.back())) rest.pop_back();
  fields.push_back(rest);
  for (size_t k = 0; k < names.size(); k++)
    sh.set(names[k], k < fields.size() ? fields[k] : std::string());
  return 0;
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
    "help", "builtin", "logout", "hash", "shopt", "ulimit", "enable", "caller", "alias", "unalias", "history", "fc", "compgen", "complete", "compopt", "bind", nullptr};

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
  size_t i = 1;
  for (; i < argv.size(); i++) {
    const std::string &a = argv[i];
    if (a.size() >= 2 && a[0] == '-') {
      for (size_t k = 1; k < a.size(); k++) {
        switch (a[k]) {
          case 'a': mk_array = true; break;
          case 'A': mk_assoc = true; break;
          case 'i': integer = true; break;
          case 'r': readonly = true; break;
          case 'x': exported = true; break;
          case 'g': global = true; break;
          case 'n': nameref = true; break;
          case 'l': lcase = true; break;
          case 'u': ucase = true; break;
          case 'p': break;
          default: break;
        }
      }
    } else {
      break;
    }
  }
  for (; i < argv.size(); i++) {
    const std::string &a = argv[i];
    size_t nend = a.find_first_of("[=");
    std::string name = (nend == std::string::npos) ? a : a.substr(0, nend);
    size_t eq = a.find('=');
    if (local && !global) sh.make_local(name);
    if (mk_assoc) sh.make_array(name, true);
    else if (mk_array) sh.make_array(name, false);
    if (eq != std::string::npos) {
      std::string val = a.substr(eq + 1);
      bool arraylit = val.size() >= 2 && val.front() == '(' && val.back() == ')';
      bool subscript = nend != std::string::npos && a[nend] == '[';
      if (arraylit || subscript) {
        apply_assignment_word(sh, a);  // NAME=(...) or NAME[i]=...
      } else {
        Expander ex(sh);
        val = ex.expand_assignment(val);  // arg arrives raw (assignment builtin)
        if (integer) { bool ok = true; val = std::to_string(eval_arith(sh, val, &ok)); }
        if (lcase) for (char &c : val) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        else if (ucase) for (char &c : val) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        sh.set(name, val);
      }
    }
    Variable &v = sh.vars[name];
    if (readonly) v.readonly = true;
    if (exported) v.exported = true;
    if (integer) v.integer = true;
    // Mark the nameref last, so the assignment above stored the *target name*
    // as this variable's value rather than being redirected through it.
    if (nameref) v.nameref = true;
  }
  return 0;
}

int bi_let(Shell &sh, const std::vector<std::string> &argv) {
  long long last = 0;
  bool ok = true;
  for (size_t i = 1; i < argv.size(); i++) last = eval_arith(sh, argv[i], &ok);
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
        default: ok = false; break;
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
    struct Loc { char kind; std::string text; };  // k/f/b/F
    std::vector<Loc> locs;
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
        std::printf("%s\n", L.kind == 'k' ? "keyword"
                          : L.kind == 'f' ? "function"
                          : L.kind == 'b' ? "builtin"
                                          : "file");
      } else if (fp) {
        if (L.kind == 'F') std::printf("%s\n", L.text.c_str());
      } else {
        switch (L.kind) {
          case 'k': std::printf("%s is a shell keyword\n", n.c_str()); break;
          case 'f':
            std::printf("%s is a function\n%s () %s\n", n.c_str(), n.c_str(),
                        to_string(sh.functions[n]).c_str());
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
    case SIGQUIT: return "QUIT"; case SIGTERM: return "TERM";
    case SIGUSR1: return "USR1"; case SIGUSR2: return "USR2";
    case SIGALRM: return "ALRM"; case SIGPIPE: return "PIPE";
    case SIGTSTP: return "TSTP"; case SIGCONT: return "CONT";
    case SIGCHLD: return "CHLD";
    default: return nullptr;
  }
}

int bi_trap(Shell &sh, const std::vector<std::string> &argv) {
  if (argv.size() < 2) {
    for (const auto &kv : sh.traps)
      std::printf("trap -- '%s' %s\n", kv.second.c_str(), kv.first.c_str());
    return 0;
  }
  std::string cmd = argv[1];
  bool reset = (cmd == "-");
  bool ignore = cmd.empty();
  for (size_t i = 2; i < argv.size(); i++) {
    std::string spec = argv[i];
    std::string upper = spec;
    if (upper.rfind("SIG", 0) == 0 || upper.rfind("sig", 0) == 0) upper = upper.substr(3);
    for (char &c : upper) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));

    // Pseudo-signals run at specific points, not on OS signal delivery.
    if (upper == "EXIT" || upper == "DEBUG" || upper == "ERR" || upper == "RETURN") {
      if (reset) sh.traps.erase(upper);
      else sh.traps[upper] = cmd;
      continue;
    }

    int signo = signame_to_num(spec);
    const char *canon = trapname_from_num(signo);
    std::string key = canon ? canon : upper;
    if (reset) {
      sh.traps.erase(key);
      sh.set_signal_trap(signo, false);
    } else if (ignore) {
      sh.traps.erase(key);
      signal(signo, SIG_IGN);
    } else {
      sh.traps[key] = cmd;
      sh.set_signal_trap(signo, true);
    }
  }
  return 0;
}

int bi_umask(const std::vector<std::string> &argv) {
  if (argv.size() < 2) {
    mode_t m = umask(0);
    umask(m);
    std::printf("%04o\n", m);
    return 0;
  }
  mode_t m = static_cast<mode_t>(std::strtol(argv[1].c_str(), nullptr, 8));
  umask(m);
  return 0;
}

int bi_getopts(Shell &sh, const std::vector<std::string> &argv) {
  if (argv.size() < 3) return 2;
  const std::string &optstring = argv[1];
  const std::string &name = argv[2];
  std::vector<std::string> args;
  if (argv.size() > 3) args.assign(argv.begin() + 3, argv.end());
  else args = sh.positional;

  int optind = 1;
  { std::string v; if (sh.get_if_set("OPTIND", v)) optind = std::atoi(v.c_str()); }
  if (optind < 1) optind = 1;
  if (optind - 1 >= static_cast<int>(args.size())) { sh.set(name, "?"); return 1; }
  const std::string &arg = args[static_cast<size_t>(optind - 1)];
  if (arg.empty() || arg[0] != '-' || arg == "-") { sh.set(name, "?"); return 1; }
  // simple: one option char per argument element (no bundling/OPTARG parsing offset)
  char opt = arg.size() > 1 ? arg[1] : '?';
  size_t pos = optstring.find(opt);
  if (pos == std::string::npos) { sh.set(name, "?"); sh.set("OPTARG", std::string(1, opt)); sh.set("OPTIND", std::to_string(optind + 1)); return 0; }
  sh.set(name, std::string(1, opt));
  if (pos + 1 < optstring.size() && optstring[pos + 1] == ':') {
    if (optind < static_cast<int>(args.size())) { sh.set("OPTARG", args[static_cast<size_t>(optind)]); optind++; }
  }
  sh.set("OPTIND", std::to_string(optind + 1));
  return 0;
}

int signame_to_num(const std::string &s) {
  if (!s.empty() && std::isdigit(static_cast<unsigned char>(s[0]))) return std::atoi(s.c_str());
  std::string n = s;
  if (n.rfind("SIG", 0) == 0) n = n.substr(3);
  for (char &c : n) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  struct { const char *name; int sig; } tbl[] = {
      {"HUP", SIGHUP},   {"INT", SIGINT},   {"QUIT", SIGQUIT}, {"KILL", SIGKILL},
      {"TERM", SIGTERM}, {"STOP", SIGSTOP}, {"CONT", SIGCONT}, {"TSTP", SIGTSTP},
      {"USR1", SIGUSR1}, {"USR2", SIGUSR2}, {"ALRM", SIGALRM}, {"CHLD", SIGCHLD},
      {"PIPE", SIGPIPE}, {nullptr, 0}};
  for (int i = 0; tbl[i].name; i++)
    if (n == tbl[i].name) return tbl[i].sig;
  return SIGTERM;
}

int bi_kill(Shell &sh, const std::vector<std::string> &argv) {
  int sig = SIGTERM;
  size_t i = 1;
  if (argv.size() > 1 && argv[1].size() > 1 && argv[1][0] == '-') {
    sig = signame_to_num(argv[1].substr(1));
    i = 2;
  }
  int st = 0;
  for (; i < argv.size(); i++) {
    const std::string &t = argv[i];
    pid_t target;
    if (!t.empty() && t[0] == '%') {
      Shell::Job *j = sh.job_by_spec(t);
      if (!j) { std::fprintf(stderr, "gnash: kill: %s: no such job\n", t.c_str()); st = 1; continue; }
      target = static_cast<pid_t>(-j->pgid);
    } else {
      target = static_cast<pid_t>(std::atol(t.c_str()));
    }
    if (kill(target, sig) != 0) { std::fprintf(stderr, "gnash: kill: %s\n", std::strerror(errno)); st = 1; }
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
    {"unalias", "unalias [-a] name [name ...]", "Remove each NAME from the list of defined aliases."},
    {"history", "history [-c] [-d offset] [n] or history -anrw [filename] or history -ps arg [arg...]", "Display or manipulate the history list."},
    {"fc", "fc [-e ename] [-lnr] [first] [last] or fc -s [pat=rep] [command]", "Display or execute commands from the history list."},
    {"compgen", "compgen [-abcdefgjksuv] [-o option] [-A action] [-G globpat] [-W wordlist] [-F function] [-C command] [-X filterpat] [-P prefix] [-S suffix] [word]", "Display possible completions depending on the options."},
    {"complete", "complete [-abcdefgjksuv] [-pr] [-DEI] [-o option] [-A action] [-G globpat] [-W wordlist] [-F function] [-C command] [-X filterpat] [-P prefix] [-S suffix] [name ...]", "Specify how arguments are to be completed by Readline."},
    {"compopt", "compopt [-o|+o option] [-DEI] [name ...]", "Modify or display completion options."},
    {"bind", "bind [-lpsvPSVX] [-m keymap] [-f filename] [-q name] [-u name] [-r keyseq] [-x keyseq:shell-command] [keyseq:readline-function or readline-command]", "Set Readline key bindings and variables."},
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
    std::printf("Use `man -k' or `info' to find out more about commands not in this list.\n\n");
    std::vector<const BuiltinHelp *> items;
    for (const auto &h : kBuiltinHelp) items.push_back(&h);
    std::sort(items.begin(), items.end(),
              [](const BuiltinHelp *a, const BuiltinHelp *b) { return std::strcmp(a->name, b->name) < 0; });
    size_t half = (items.size() + 1) / 2;
    for (size_t r = 0; r < half; r++) {
      std::string left = items[r]->synopsis;
      std::string right = (r + half < items.size()) ? items[r + half]->synopsis : "";
      std::printf(" %-36.36s%s\n", left.c_str(), right.c_str());
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
  std::vector<std::string> sub(argv.begin() + 1, argv.end());
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
  if (!ppath.empty() && i < argv.size()) { sh.hashed[argv[i]] = ppath; return 0; }
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
    if (del_d) { sh.hashed.erase(n); continue; }
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

void shopt_seed(Shell &sh) {
  if (!sh.shopt_opts.empty()) return;
  for (const auto &o : kShoptDefaults) sh.shopt_opts[o.name] = o.on;
  sh.shopt_opts["login_shell"] = sh.login_shell;
}

int bi_shopt(Shell &sh, const std::vector<std::string> &argv) {
  shopt_seed(sh);
  bool set_s = false, unset_u = false, quiet_q = false, print_p = false;
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
        case 'o': break;  // -o maps to set -o options; accepted
        default: break;
      }
    }
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

static void ulimit_print_one(const UlimitRes &r, bool hard) {
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
  char unitstr[32];
  if (r.unit) std::snprintf(unitstr, sizeof unitstr, "(%s, -%c) ", r.unit, r.opt);
  else std::snprintf(unitstr, sizeof unitstr, "(-%c) ", r.opt);
  std::printf("%-20s %20s%s\n", r.desc, unitstr, val.c_str());
}

int bi_ulimit(Shell &sh, const std::vector<std::string> &argv) {
  bool hard = false, soft = false, all = false;
  char opt = 'f';  // default resource
  std::string value;
  bool have_opt = false;
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
      else { opt = o; have_opt = true; }
    }
  }
  if (i < argv.size()) value = argv[i];
  if (!hard && !soft) soft = true;  // default acts on the soft limit

  if (all) {
    for (const auto &r : kUlimits) ulimit_print_one(r, hard);
    return 0;
  }

  const UlimitRes *r = nullptr;
  for (const auto &e : kUlimits) if (e.opt == opt) { r = &e; break; }
  if (!r) { std::fprintf(stderr, "%sulimit: -%c: invalid option\n", sh.err_prefix().c_str(), opt); return 2; }
  (void)have_opt;

  if (value.empty()) {  // report
    ulimit_print_one(*r, hard);
    return 0;
  }
  if (r->res < 0) return 0;  // pipe size is not settable
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
  return 0;
}

// ---- enable --------------------------------------------------------------
int bi_enable(Shell &sh, const std::vector<std::string> &argv) {
  bool disable = false, all = false;
  size_t i = 1;
  for (; i < argv.size(); i++) {
    const std::string &a = argv[i];
    if (a == "--") { i++; break; }
    if (a.size() < 2 || a[0] != '-') break;
    for (size_t k = 1; k < a.size(); k++) {
      if (a[k] == 'n') disable = true;
      else if (a[k] == 'a') all = true;
      else if (a[k] == 'p') { /* posix reusable list: like default */ }
    }
  }
  if (i >= argv.size()) {  // list
    for (const std::string &nm : builtin_names_sorted()) {
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
  if (sh.call_stack.empty()) return 1;
  size_t top = sh.call_stack.size() - 1;
  // call_stack[k].line is where call_stack[k].func was invoked.  For frame N,
  // bash reports that call line, but the *calling* function's name/source (the
  // frame just below), FUNCNAME[N+1]/BASH_SOURCE[N+1].
  if (argv.size() > 1) {  // caller N: "line function source"
    long n = std::atol(argv[1].c_str());
    // Valid only while FUNCNAME[N+1] is a real function (not the main script).
    if (n < 0 || static_cast<size_t>(n) + 1 >= sh.call_stack.size()) return 1;
    const auto &fr = sh.call_stack[top - static_cast<size_t>(n)];
    const auto &caller = sh.call_stack[top - static_cast<size_t>(n) - 1];
    std::printf("%d %s %s\n", fr.line, caller.func.c_str(), caller.source.c_str());
    return 0;
  }
  const auto &fr = sh.call_stack[top];  // caller (no arg): "line source"
  std::string source = (top >= 1) ? sh.call_stack[top - 1].source : fr.source;
  std::printf("%d %s\n", fr.line, source.c_str());
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
  if (i < argv.size() && (argv[i] == "-p")) i++;
  if (i >= argv.size()) {
    for (const auto &kv : sh.aliases)
      std::printf("alias %s=%s\n", kv.first.c_str(), alias_quote(kv.second).c_str());
    return 0;
  }
  int st = 0;
  for (; i < argv.size(); i++) {
    const std::string &a = argv[i];
    auto eq = a.find('=');
    if (eq == std::string::npos) {
      auto it = sh.aliases.find(a);
      if (it != sh.aliases.end())
        std::printf("alias %s=%s\n", it->first.c_str(), alias_quote(it->second).c_str());
      else {
        std::fflush(stdout);
        std::fprintf(stderr, "%salias: %s: not found\n", sh.err_prefix().c_str(), a.c_str());
        st = 1;
      }
    } else {
      sh.aliases[a.substr(0, eq)] = a.substr(eq + 1);
    }
  }
  return st;
}

int bi_unalias(Shell &sh, const std::vector<std::string> &argv) {
  if (argv.size() > 1 && argv[1] == "-a") { sh.aliases.clear(); return 0; }
  int st = 0;
  for (size_t i = 1; i < argv.size(); i++) {
    if (sh.aliases.erase(argv[i]) == 0) {
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
  int limit = -1;
  size_t i = 1;
  for (; i < argv.size(); i++) {
    const std::string &a = argv[i];
    if (a == "-c") { clear_history(); return 0; }
    if (a == "-d") {
      if (i + 1 < argv.size()) remove_history(std::atoi(argv[i + 1].c_str()) - history_base);
      return 0;
    }
    if (a == "-s") {
      std::string s = join(argv, i + 1);
      if (!s.empty()) add_history(s.c_str());
      return 0;
    }
    if (a == "-p") {
      for (size_t k = i + 1; k < argv.size(); k++) {
        char *e = nullptr;
        history_expand(const_cast<char *>(argv[k].c_str()), &e);
        if (e) { std::printf("%s\n", e); std::free(e); }
      }
      return 0;
    }
    if (a == "-w") { write_history((i + 1 < argv.size() ? argv[i + 1] : hist_file(sh)).c_str()); return 0; }
    if (a == "-a") { append_history(0, (i + 1 < argv.size() ? argv[i + 1] : hist_file(sh)).c_str()); return 0; }
    if (a == "-r" || a == "-n") { read_history((i + 1 < argv.size() ? argv[i + 1] : hist_file(sh)).c_str()); return 0; }
    if (!a.empty() && (std::isdigit(static_cast<unsigned char>(a[0])))) { limit = std::atoi(a.c_str()); continue; }
    break;
  }
  HIST_ENTRY **list = history_list();
  if (!list) return 0;
  int n = 0;
  while (list[n]) n++;
  int start = (limit > 0 && limit < n) ? n - limit : 0;
  for (int k = start; k < n; k++)
    std::printf("%5d  %s\n", history_base + k, list[k]->line);
  return 0;
}

int bi_fc(Shell &sh, const std::vector<std::string> &argv) {
  bool list = false, nonum = false, reverse = false, subst = false;
  size_t i = 1;
  for (; i < argv.size(); i++) {
    const std::string &a = argv[i];
    if (a.size() < 2 || a[0] != '-') break;
    if (a == "--") { i++; break; }
    for (size_t k = 1; k < a.size(); k++) {
      if (a[k] == 'l') list = true;
      else if (a[k] == 'n') nonum = true;
      else if (a[k] == 'r') reverse = true;
      else if (a[k] == 's') subst = true;
      else if (a[k] == 'e') { if (i + 1 < argv.size()) i++; }  // editor: ignored
    }
  }
  HIST_ENTRY **hl = history_list();
  int n = 0;
  if (hl) while (hl[n]) n++;
  if (n == 0) return 0;

  if (subst) {  // fc -s [old=new] [command]: re-run a previous command
    std::string oldpat, newpat, match;
    for (; i < argv.size(); i++) {
      auto eq = argv[i].find('=');
      if (eq != std::string::npos && oldpat.empty()) { oldpat = argv[i].substr(0, eq); newpat = argv[i].substr(eq + 1); }
      else match = argv[i];
    }
    int idx = n - 1;  // most recent
    if (!match.empty())
      for (int k = n - 1; k >= 0; k--)
        if (std::strncmp(hl[k]->line, match.c_str(), match.size()) == 0) { idx = k; break; }
    std::string cmd = hl[idx]->line;
    if (!oldpat.empty()) {
      auto p = cmd.find(oldpat);
      if (p != std::string::npos) cmd.replace(p, oldpat.size(), newpat);
    }
    std::printf("%s\n", cmd.c_str());
    add_history(cmd.c_str());
    return sh.run_string(cmd);
  }

  // fc -l [first] [last]: list a range (default last 16).
  int first = n - 16, last = n - 1;
  std::vector<std::string> nums;
  for (; i < argv.size(); i++) nums.push_back(argv[i]);
  if (nums.size() >= 1) first = std::atoi(nums[0].c_str()) - history_base;
  if (nums.size() >= 2) last = std::atoi(nums[1].c_str()) - history_base;
  if (first < 0) first = 0;
  if (last >= n) last = n - 1;
  if (!list) list = true;  // default action without an editor is to list here
  auto emit = [&](int k) {
    if (nonum) std::printf("%s\n", hl[k]->line);
    else std::printf("%d\t %s\n", history_base + k, hl[k]->line);
  };
  if (reverse) for (int k = last; k >= first; k--) emit(k);
  else for (int k = first; k <= last; k++) emit(k);
  return 0;
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

// Collect the raw candidate list for compgen's actions (before prefix filter).
static void compgen_collect(Shell &sh, const std::vector<char> &actions, const std::string &word,
                            std::vector<std::string> &c) {
  for (char a : actions) {
    switch (a) {
      case 'b': for (const auto &n : builtin_names_sorted()) c.push_back(n); break;
      case 'k': for (int i = 0; kReservedWords[i]; i++) c.push_back(kReservedWords[i]); break;
      case 'v': for (const auto &kv : sh.vars) c.push_back(kv.first); break;
      case 'e': for (const auto &kv : sh.vars) if (kv.second.exported) c.push_back(kv.first); break;
      case 'a': for (const auto &kv : sh.aliases) c.push_back(kv.first); break;
      case 'F': for (const auto &kv : sh.functions) c.push_back(kv.first); break;
      case 'f': compgen_files(word, false, c); break;
      case 'd': compgen_files(word, true, c); break;
      case 'c': {  // command names: keywords, builtins, functions, aliases, PATH
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
        break;
      }
      default: break;
    }
  }
}

int bi_compgen(Shell &sh, const std::vector<std::string> &argv) {
  std::vector<std::string> words;
  std::vector<char> actions;
  std::string prefix, suffix, word;
  size_t i = 1;
  auto optarg = [&](const std::string &a, size_t &k) -> std::string {
    if (a.size() > 2) return a.substr(2);
    return (i + 1 < argv.size()) ? argv[++i] : std::string();
    (void)k;
  };
  for (; i < argv.size(); i++) {
    const std::string &a = argv[i];
    if (a == "--") { i++; break; }
    if (a.size() < 2 || a[0] != '-') break;
    char o = a[1];
    size_t k = 0;
    if (o == 'W') { std::istringstream iss(optarg(a, k)); std::string w; while (iss >> w) words.push_back(w); }
    else if (o == 'P') prefix = optarg(a, k);
    else if (o == 'S') suffix = optarg(a, k);
    else if (o == 'A') {
      std::string act = optarg(a, k);
      if (act == "function") actions.push_back('F');
      else if (act == "builtin") actions.push_back('b');
      else if (act == "keyword") actions.push_back('k');
      else if (act == "variable") actions.push_back('v');
      else if (act == "export") actions.push_back('e');
      else if (act == "alias") actions.push_back('a');
      else if (act == "command") actions.push_back('c');
      else if (act == "file") actions.push_back('f');
      else if (act == "directory") actions.push_back('d');
    } else if (std::strchr("bkvecafd", o)) {
      for (size_t j = 1; j < a.size(); j++) actions.push_back(a[j]);
    } else if (std::strchr("GXoFC", o)) {
      optarg(a, k);  // consume the argument of unsupported generators
    }
  }
  if (i < argv.size()) word = argv[i];

  std::vector<std::string> cands = words;
  compgen_collect(sh, actions, word, cands);

  int printed = 0;
  for (const std::string &c : cands) {
    if (!word.empty() && c.compare(0, word.size(), word) != 0) continue;
    std::printf("%s%s%s\n", prefix.c_str(), c.c_str(), suffix.c_str());
    printed++;
  }
  return printed ? 0 : 1;
}

// Reconstruct a `complete' spec string (best effort) for -p output.
int bi_complete(Shell &sh, const std::vector<std::string> &argv) {
  bool print = false, remove = false;
  std::string spec;  // the option part, reused as the stored spec
  size_t i = 1;
  std::vector<std::string> names;
  for (; i < argv.size(); i++) {
    const std::string &a = argv[i];
    if (a == "-p") { print = true; continue; }
    if (a == "-r") { remove = true; continue; }
    if (a.size() >= 2 && a[0] == '-') {
      spec += (spec.empty() ? "" : " ") + a;
      // options that take an argument
      if (std::strchr("WFCGXPSAo", a[1]) && a.size() == 2 && i + 1 < argv.size()) {
        std::string arg = argv[++i];
        spec += " " + (a[1] == 'W' || a[1] == 'F' || a[1] == 'C' ? "'" + arg + "'" : arg);
      }
      continue;
    }
    names.push_back(a);
  }
  if (remove) {
    if (names.empty()) sh.completions.clear();
    else for (const auto &n : names) sh.completions.erase(n);
    return 0;
  }
  if (print || names.empty()) {
    if (names.empty()) {
      for (const auto &kv : sh.completions)
        std::printf("complete %s %s\n", kv.second.c_str(), kv.first.c_str());
      return 0;
    }
    int st = 0;
    for (const auto &n : names) {
      auto it = sh.completions.find(n);
      if (it != sh.completions.end())
        std::printf("complete %s %s\n", it->second.c_str(), n.c_str());
      else {
        std::fflush(stdout);
        std::fprintf(stderr, "%scomplete: %s: no completion specification\n",
                     sh.err_prefix().c_str(), n.c_str());
        st = 1;
      }
    }
    return st;
  }
  for (const auto &n : names) sh.completions[n] = spec;
  return 0;
}

int bi_compopt(Shell &sh, const std::vector<std::string> &) {
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
        "ttyctl", "sched", "zcompile", "emulate", "add-zsh-hook", "zrecompile"};
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
  else if (cmd == "read") st = bi_read(sh, argv);
  else if (cmd == "test") st = bi_test(sh, argv, false);
  else if (cmd == "[") st = bi_test(sh, argv, true);
  else if (cmd == "shift") {
    int n = argv.size() > 1 ? std::atoi(argv[1].c_str()) : 1;
    for (int k = 0; k < n && !sh.positional.empty(); k++) sh.positional.erase(sh.positional.begin());
    st = 0;
  } else if (cmd == "exit") {
    sh.exiting = true;
    sh.exit_status = argv.size() > 1 ? (std::atoi(argv[1].c_str()) & 0xff) : sh.last_status;
    st = sh.exit_status;
  } else if (cmd == "return") {
    sh.returning = true;
    sh.exit_status = argv.size() > 1 ? (std::atoi(argv[1].c_str()) & 0xff) : sh.last_status;
    st = sh.exit_status;
  } else if (cmd == "break") {
    sh.break_count = argv.size() > 1 ? std::atoi(argv[1].c_str()) : 1;
    st = 0;
  } else if (cmd == "continue") {
    sh.continue_count = argv.size() > 1 ? std::atoi(argv[1].c_str()) : 1;
    st = 0;
  } else if (cmd == "eval") {
    st = sh.run_string(join(argv, 1));
  } else if (cmd == "source" || cmd == ".") {
    if (argv.size() > 1) {
      // Like bash: a filename without a slash is looked up on PATH first, then
      // (as a fallback) in the current directory.
      std::string path = argv[1];
      if (path.find('/') == std::string::npos && access(path.c_str(), R_OK) != 0) {
        const char *penv = std::getenv("PATH");
        std::string ps = penv ? penv : "";
        size_t start = 0;
        while (start <= ps.size()) {
          size_t e = ps.find(':', start);
          std::string dir = ps.substr(start, e == std::string::npos ? std::string::npos : e - start);
          if (dir.empty()) dir = ".";
          std::string cand = dir + "/" + argv[1];
          if (access(cand.c_str(), R_OK) == 0) { path = cand; break; }
          if (e == std::string::npos) break;
          start = e + 1;
        }
      }
      std::ifstream f(path);
      if (f) {
        std::ostringstream ss; ss << f.rdbuf();
        // A sourced file becomes the innermost BASH_SOURCE frame; use the
        // resolved path (bash records the PATH-found path, not the bare name),
        // so ${BASH_SOURCE[0]} lets a script locate itself.  The call line is
        // where `source' appears in the current file.
        sh.push_src_frame("source", path, sh.cur_lineno, false);
        st = sh.run_string(ss.str());
        sh.pop_src_frame();
      }
      else { std::fprintf(stderr, "gnash: %s: %s\n", argv[1].c_str(), std::strerror(errno)); st = 1; }
    }
  } else if (cmd == "local") {
    st = bi_declare(sh, argv, true, false);
  } else if (cmd == "declare" || cmd == "typeset") {
    st = bi_declare(sh, argv, false, false);
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
    st = bi_umask(argv);
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
    Shell::Job *j = sh.job_by_spec(argv.size() > 1 ? argv[1] : "");
    if (j) st = sh.foreground_job(*j, true);
    else { std::fprintf(stderr, "gnash: fg: no current job\n"); st = 1; }
  } else if (cmd == "bg") {
    Shell::Job *j = sh.job_by_spec(argv.size() > 1 ? argv[1] : "");
    if (j) { sh.background_job(*j, true); st = 0; }
    else { std::fprintf(stderr, "gnash: bg: no current job\n"); st = 1; }
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
    if (argv.size() > 1 && argv[1] == "-v") {
      st = 1;
      if (argv.size() > 2) {
        std::string n = argv[2];
        if (sh.functions.count(n) || is_builtin_name(n)) { std::printf("%s\n", n.c_str()); st = 0; }
        else { std::string p = find_in_path(sh, n); if (!p.empty()) { std::printf("%s\n", p.c_str()); st = 0; } }
      }
    } else {
      st = sh.run_string(join(argv, 1));
    }
  } else if (cmd == "exec") {
    if (argv.size() > 1) {
      std::vector<char *> cargv;
      for (size_t i = 1; i < argv.size(); i++) cargv.push_back(const_cast<char *>(argv[i].c_str()));
      cargv.push_back(nullptr);
      execvp(cargv[0], cargv.data());
      std::fprintf(stderr, "gnash: exec: %s: not found\n", argv[1].c_str());
      _exit(127);
    }
    st = 0;
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
        long l = std::strtol(lhs.c_str(), nullptr, 10);
        long r = std::strtol(expand(rhs_raw).c_str(), nullptr, 10);
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
  if (status) *status = v ? 0 : 1;
  return v;
}

}  // namespace gnash::core
