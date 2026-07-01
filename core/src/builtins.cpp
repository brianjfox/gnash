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
#include <sstream>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "gnash/core/expand.hpp"
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

int bi_cd(Shell &sh, const std::vector<std::string> &argv) {
  std::string dir;
  if (argv.size() < 2) dir = sh.get("HOME");
  else if (argv[1] == "-") { dir = sh.get("OLDPWD"); std::printf("%s\n", dir.c_str()); }
  else dir = argv[1];
  if (dir.empty()) return 1;
  char oldpwd[4096];
  if (!getcwd(oldpwd, sizeof oldpwd)) oldpwd[0] = '\0';
  if (chdir(dir.c_str()) != 0) {
    std::fprintf(stderr, "gnash: cd: %s: %s\n", dir.c_str(), std::strerror(errno));
    return 1;
  }
  sh.set_exported("OLDPWD", oldpwd);
  char newpwd[4096];
  if (getcwd(newpwd, sizeof newpwd)) sh.set_exported("PWD", newpwd);
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

int bi_set(Shell &sh, const std::vector<std::string> &argv) {
  size_t i = 1;
  bool positional_given = false;
  for (; i < argv.size(); i++) {
    const std::string &a = argv[i];
    if (a == "--") { i++; positional_given = true; break; }
    if (a.size() >= 2 && (a[0] == '-' || a[0] == '+')) {
      bool on = a[0] == '-';
      for (size_t k = 1; k < a.size(); k++) {
        switch (a[k]) {
          case 'e': sh.opt_errexit = on; break;
          case 'x': sh.opt_xtrace = on; break;
          case 'u': sh.opt_nounset = on; break;
          case 'f': sh.opt_noglob = on; break;
          case 'v': sh.opt_verbose = on; break;
          case 'o': {
            if (i + 1 < argv.size()) {
              std::string o = argv[++i];
              if (o == "errexit") sh.opt_errexit = on;
              else if (o == "xtrace") sh.opt_xtrace = on;
              else if (o == "nounset") sh.opt_nounset = on;
              else if (o == "noglob") sh.opt_noglob = on;
              else if (o == "verbose") sh.opt_verbose = on;
            }
            break;
          }
          default: break;
        }
      }
    } else {
      positional_given = true;
      break;
    }
  }
  if (positional_given) {
    sh.positional.assign(argv.begin() + static_cast<long>(i), argv.end());
  }
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

bool is_builtin_name(const std::string &n) {
  static const char *names[] = {
      ":", "true", "false", "echo", "printf", "pwd", "cd", "export", "unset", "set",
      "read", "test", "[", "shift", "exit", "return", "break", "continue", "eval",
      "source", ".", "local", "declare", "typeset", "readonly", "let", "type", "trap",
      "umask", "getopts", "exec", "command", "times", "wait", "jobs", "fg", "bg",
      "disown", "kill", "suspend", nullptr};
  for (int i = 0; names[i]; i++)
    if (n == names[i]) return true;
  return false;
}

// Shared logic for declare/local/readonly/typeset.
int bi_declare(Shell &sh, const std::vector<std::string> &argv, bool force_local, bool force_ro) {
  bool mk_array = false, mk_assoc = false, integer = false, readonly = force_ro;
  bool exported = false, global = false, local = force_local;
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
    size_t eq = a.find('=');
    std::string name = eq == std::string::npos ? a : a.substr(0, eq);
    if (local && !global) sh.make_local(name);
    if (mk_assoc) sh.make_array(name, true);
    else if (mk_array) sh.make_array(name, false);
    if (eq != std::string::npos) {
      std::string val = a.substr(eq + 1);
      if (integer) { bool ok = true; val = std::to_string(eval_arith(sh, val, &ok)); }
      sh.set(name, val);
    }
    Variable &v = sh.vars[name];
    if (readonly) v.readonly = true;
    if (exported) v.exported = true;
    if (integer) v.integer = true;
  }
  return 0;
}

int bi_let(Shell &sh, const std::vector<std::string> &argv) {
  long long last = 0;
  bool ok = true;
  for (size_t i = 1; i < argv.size(); i++) last = eval_arith(sh, argv[i], &ok);
  return (ok && last != 0) ? 0 : 1;
}

int bi_type(Shell &sh, const std::vector<std::string> &argv) {
  int st = 0;
  for (size_t i = 1; i < argv.size(); i++) {
    const std::string &n = argv[i];
    if (sh.functions.count(n)) std::printf("%s is a function\n", n.c_str());
    else if (is_builtin_name(n)) std::printf("%s is a shell builtin\n", n.c_str());
    else {
      std::string p = find_in_path(sh, n);
      if (!p.empty()) std::printf("%s is %s\n", n.c_str(), p.c_str());
      else { std::fprintf(stderr, "gnash: type: %s: not found\n", n.c_str()); st = 1; }
    }
  }
  return st;
}

int bi_trap(Shell &sh, const std::vector<std::string> &argv) {
  if (argv.size() < 2) {
    for (const auto &kv : sh.traps)
      std::printf("trap -- '%s' %s\n", kv.second.c_str(), kv.first.c_str());
    return 0;
  }
  std::string cmd = argv[1];
  size_t start = 2;
  bool remove = (cmd == "-");
  for (size_t i = start; i < argv.size(); i++) {
    std::string sig = argv[i];
    if (sig.rfind("SIG", 0) == 0) sig = sig.substr(3);
    for (char &c : sig) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    if (remove) sh.traps.erase(sig);
    else sh.traps[sig] = cmd;
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

}  // namespace

// [[ ]] evaluation over the reconstructed expression (re-tokenized).
bool eval_cond_expression(Shell &sh, const std::string &expr, int *status);

bool run_builtin(Shell &sh, const std::vector<std::string> &argv, int *status) {
  if (argv.empty()) return false;
  const std::string &cmd = argv[0];
  int st = 0;

  if (cmd == ":" || cmd == "true") st = 0;
  else if (cmd == "false") st = 1;
  else if (cmd == "echo") st = bi_echo(sh, argv);
  else if (cmd == "printf") st = bi_printf(sh, argv);
  else if (cmd == "pwd") { char b[4096]; if (getcwd(b, sizeof b)) std::printf("%s\n", b); st = 0; }
  else if (cmd == "cd") st = bi_cd(sh, argv);
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
      std::ifstream f(argv[1]);
      if (f) { std::ostringstream ss; ss << f.rdbuf(); st = sh.run_string(ss.str()); }
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
          regex_t rx;
          if (regcomp(&rx, re.c_str(), REG_EXTENDED) != 0) return false;
          bool m = regexec(&rx, lhs.c_str(), 0, nullptr, 0) == 0;
          regfree(&rx);
          return m;
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
