// csh.cpp -- a csh/tcsh interpreter for gnash's csh personality.
//
// csh is not a Bourne-family shell, so this is a separate front end: a
// hand-written scanner + recursive-descent parser over the source, and a
// tree-walking evaluator.  It covers the common script constructs: simple
// commands (pipelines, redirection, background), the `set'/`@'/`setenv'
// builtins, $-substitution (incl. $var[i], $#var, $?var), backquote command
// substitution, filename globbing, `if (...) then/else/endif' (and one-line
// form), `foreach', `while', `switch', and the `(expr)' expression language.

#include "gnash/core/csh.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <string>
#include <vector>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "gnash/core/expand.hpp"
#include "gnash/glob.hpp"
#include "strmatch.h"

extern "C" char **environ;

namespace gnash::core {
namespace {

// ---- AST ------------------------------------------------------------------

struct Redir { char op; std::string target; };  // '<', '>', 'A'(>>), 'E'(>&)
struct Cmd { std::vector<std::string> words; std::vector<Redir> redirs; };
struct Pipeline { std::vector<Cmd> cmds; bool background = false; };

struct Stmt;
using StmtPtr = std::unique_ptr<Stmt>;
struct CaseArm { std::string pattern; bool is_default = false; std::vector<StmtPtr> body; };

enum class Kind { Simple, If, Foreach, While, Switch, Break, Continue, Breaksw };
struct Stmt {
  Kind kind;
  Pipeline pipe;                       // Simple
  std::string expr;                    // If / While condition (raw)
  std::string var;                     // Foreach variable
  std::vector<std::string> list;       // Foreach words (raw)
  std::vector<StmtPtr> body;           // Foreach / While body
  std::vector<StmtPtr> then_body;      // If then
  std::vector<StmtPtr> else_body;      // If else
  std::string switch_word;             // Switch subject (raw)
  std::vector<CaseArm> arms;           // Switch arms
};

// ---- scanner + parser -----------------------------------------------------

struct Parser {
  const std::string &s;
  size_t p = 0;
  bool err = false;
  explicit Parser(const std::string &src) : s(src) {}

  bool eof() const { return p >= s.size(); }
  char cur() const { return p < s.size() ? s[p] : '\0'; }

  void skip_ws() {
    while (p < s.size()) {
      if (s[p] == ' ' || s[p] == '\t') p++;
      else if (s[p] == '\\' && p + 1 < s.size() && s[p + 1] == '\n') p += 2;  // continuation
      else break;
    }
  }
  void skip_blank_lines() {
    for (;;) {
      skip_ws();
      if (cur() == '#') { while (p < s.size() && s[p] != '\n') p++; }
      if (cur() == '\n' || cur() == ';') { p++; continue; }
      break;
    }
  }
  bool is_meta(char c) {
    // Note: '#' is NOT here -- it starts a comment only at the beginning of a
    // word (handled by the callers), so mid-word '#' (as in $#x) stays literal.
    return c == ' ' || c == '\t' || c == '\n' || c == ';' || c == '|' ||
           c == '&' || c == '<' || c == '>' || c == '(' || c == ')';
  }

  // Read one word (raw, quotes preserved) up to the next metacharacter.
  std::string read_word() {
    std::string w;
    while (p < s.size()) {
      char c = s[p];
      if (c == '\'' ) { w += c; p++; while (p < s.size() && s[p] != '\'') w += s[p++]; if (p < s.size()) w += s[p++]; continue; }
      if (c == '"') { w += c; p++; while (p < s.size() && s[p] != '"') { if (s[p] == '\\' && p + 1 < s.size()) w += s[p++]; w += s[p++]; } if (p < s.size()) w += s[p++]; continue; }
      if (c == '`') { w += c; p++; while (p < s.size() && s[p] != '`') w += s[p++]; if (p < s.size()) w += s[p++]; continue; }
      if (c == '\\' && p + 1 < s.size()) { w += c; w += s[p + 1]; p += 2; continue; }
      if (is_meta(c)) break;
      w += c;
      p++;
    }
    return w;
  }

  // Raw text of a balanced (...) group; p must be at '('.  Consumes through ')'.
  std::string read_paren() {
    std::string out;
    if (cur() != '(') return out;
    p++;  // past '('
    int depth = 1;
    while (p < s.size() && depth > 0) {
      char c = s[p];
      if (c == '\'' || c == '"' || c == '`') {
        char q = c; out += c; p++;
        while (p < s.size() && s[p] != q) { out += s[p++]; }
        if (p < s.size()) out += s[p++];
        continue;
      }
      if (c == '(') depth++;
      else if (c == ')') { depth--; if (depth == 0) { p++; break; } }
      out += c;
      p++;
    }
    return out;
  }

  std::string peek_word() { size_t save = p; skip_ws(); std::string w = read_word(); p = save; return w; }

  Pipeline parse_pipeline() {
    Pipeline pl;
    Cmd cmd;
    for (;;) {
      skip_ws();
      char c = cur();
      if (c == '\0' || c == '\n' || c == ';') break;
      if (c == '#') { while (p < s.size() && s[p] != '\n') p++; break; }
      if (c == '&') { p++; if (cur() == '&') { p++; /* && handled at stmt level via re-scan */ p -= 2; break; } pl.background = true; break; }
      if (c == '|') { p++; if (cur() == '|') { p -= 1; break; } pl.cmds.push_back(cmd); cmd = Cmd{}; continue; }
      if (c == '<') { p++; skip_ws(); cmd.redirs.push_back({'<', read_word()}); continue; }
      if (c == '>') {
        p++; char op = '>';
        if (cur() == '>') { p++; op = 'A'; }
        if (cur() == '&') { p++; op = (op == 'A') ? 'A' : 'E'; }  // >& / >>&
        skip_ws(); cmd.redirs.push_back({op, read_word()}); continue;
      }
      if (c == '(') { p++; cmd.words.push_back("("); continue; }  // `set x = (list)'
      if (c == ')') { p++; cmd.words.push_back(")"); continue; }
      std::string w = read_word();
      if (w.empty()) { p++; continue; }
      cmd.words.push_back(w);
    }
    pl.cmds.push_back(cmd);
    return pl;
  }

  bool at_kw(const char *kw) {
    size_t save = p; skip_ws(); std::string w = read_word(); p = save;
    return w == kw;
  }

  std::vector<StmtPtr> parse_block(std::initializer_list<const char *> terms) {
    std::vector<StmtPtr> out;
    for (;;) {
      skip_blank_lines();
      if (eof()) break;
      std::string w = peek_word();
      bool stop = false;
      for (const char *t : terms) if (w == t) stop = true;
      if (stop) break;
      StmtPtr st = parse_stmt();
      if (st) out.push_back(std::move(st));
      if (err) break;
    }
    return out;
  }

  StmtPtr parse_stmt() {
    skip_ws();
    std::string w = peek_word();
    if (w == "if") return parse_if();
    if (w == "foreach") return parse_foreach();
    if (w == "while") return parse_while();
    if (w == "switch") return parse_switch();
    if (w == "break") { read_word(); auto st = std::make_unique<Stmt>(); st->kind = Kind::Break; return st; }
    if (w == "continue") { read_word(); auto st = std::make_unique<Stmt>(); st->kind = Kind::Continue; return st; }
    if (w == "breaksw") { read_word(); auto st = std::make_unique<Stmt>(); st->kind = Kind::Breaksw; return st; }
    auto st = std::make_unique<Stmt>();
    st->kind = Kind::Simple;
    st->pipe = parse_pipeline();
    // consume `&&'/`||' by folding into sequential execution (approximate)
    return st;
  }

  StmtPtr parse_if() {
    read_word();  // if
    skip_ws();
    std::string cond = read_paren();
    skip_ws();
    auto st = std::make_unique<Stmt>();
    st->kind = Kind::If;
    st->expr = cond;
    if (at_kw("then")) {
      read_word();  // then
      st->then_body = parse_block({"else", "endif"});
      while (at_kw("else")) {
        read_word();  // else
        skip_ws();
        if (at_kw("if")) {  // else if (...) then ...
          StmtPtr nested = parse_if_tail();
          st->else_body.push_back(std::move(nested));
          return st;
        }
        st->else_body = parse_block({"endif"});
      }
      if (at_kw("endif")) read_word();
    } else {
      // one-line: if (expr) command  (command may be break/continue/etc.)
      st->then_body.push_back(parse_stmt());
    }
    return st;
  }
  StmtPtr parse_if_tail() {  // for `else if'
    return parse_if();
  }

  StmtPtr parse_foreach() {
    read_word();  // foreach
    skip_ws();
    auto st = std::make_unique<Stmt>();
    st->kind = Kind::Foreach;
    st->var = read_word();
    skip_ws();
    std::string raw = read_paren();
    Parser sub(raw);
    for (;;) { sub.skip_ws(); if (sub.eof()) break; std::string x = sub.read_word(); if (x.empty()) { sub.p++; continue; } st->list.push_back(x); }
    st->body = parse_block({"end"});
    if (at_kw("end")) read_word();
    return st;
  }

  StmtPtr parse_while() {
    read_word();  // while
    skip_ws();
    auto st = std::make_unique<Stmt>();
    st->kind = Kind::While;
    st->expr = read_paren();
    st->body = parse_block({"end"});
    if (at_kw("end")) read_word();
    return st;
  }

  StmtPtr parse_switch() {
    read_word();  // switch
    skip_ws();
    auto st = std::make_unique<Stmt>();
    st->kind = Kind::Switch;
    st->switch_word = read_paren();
    for (;;) {
      skip_blank_lines();
      if (eof() || at_kw("endsw")) break;
      std::string w = peek_word();
      if (w == "case") {
        read_word();
        CaseArm arm;
        skip_ws();
        std::string pat;  // up to ':'
        while (p < s.size() && s[p] != ':' && s[p] != '\n') pat += s[p++];
        if (cur() == ':') p++;
        // trim
        size_t a = pat.find_first_not_of(" \t"), b = pat.find_last_not_of(" \t");
        arm.pattern = (a == std::string::npos) ? "" : pat.substr(a, b - a + 1);
        arm.body = parse_block({"case", "default", "endsw"});
        st->arms.push_back(std::move(arm));
      } else if (w == "default") {
        read_word();
        if (cur() == ':') p++;
        CaseArm arm; arm.is_default = true;
        arm.body = parse_block({"case", "default", "endsw"});
        st->arms.push_back(std::move(arm));
      } else {
        break;
      }
    }
    if (at_kw("endsw")) read_word();
    return st;
  }
};

// ---- interpreter ----------------------------------------------------------

struct Interp {
  Shell &sh;
  int status = 0;
  bool exiting = false;
  int exit_code = 0;
  int loop_ctrl = 0;  // 1=break, 2=continue, 3=breaksw
  explicit Interp(Shell &s) : sh(s) {}

  std::vector<std::string> *var_ptr(const std::string &n) {
    auto it = sh.csh_vars.find(n);
    return it == sh.csh_vars.end() ? nullptr : &it->second;
  }
  std::string var_str(const std::string &n) {  // first-word/scalar view
    if (n == "status" || n == "?") return std::to_string(status);
    if (n == "0") return sh.arg0;
    auto *v = var_ptr(n);
    if (v) { std::string r; for (size_t i = 0; i < v->size(); i++) { if (i) r += ' '; r += (*v)[i]; } return r; }
    const char *e = std::getenv(n.c_str());
    return e ? e : "";
  }
  bool var_set(const std::string &n) {
    if (n == "status" || n == "?" || n == "0") return true;
    return var_ptr(n) != nullptr || std::getenv(n.c_str()) != nullptr;
  }

  // -- expansion --
  // Expand one raw word into zero or more fields.  When do_glob is false the
  // fields are returned verbatim (used for switch/case patterns, which are
  // globbing patterns to match against, not filenames to expand).
  void expand_word(const std::string &w, std::vector<std::string> &out, bool do_glob = true) {
    std::string cur;
    bool have = false;
    std::vector<std::string> fields;
    auto push_char = [&](char c) { cur += c; have = true; };
    auto flush = [&]() { if (have) { fields.push_back(cur); cur.clear(); have = false; } };

    for (size_t i = 0; i < w.size();) {
      char c = w[i];
      if (c == '\'') { i++; while (i < w.size() && w[i] != '\'') push_char(w[i++]); if (i < w.size()) i++; have = true; continue; }
      if (c == '"') {
        i++; have = true;
        while (i < w.size() && w[i] != '"') {
          if (w[i] == '$') { std::string sub; i = expand_dollar(w, i, sub); cur += sub; }
          else if (w[i] == '\\' && i + 1 < w.size()) { push_char(w[i + 1]); i += 2; }
          else push_char(w[i++]);
        }
        if (i < w.size()) i++;
        continue;
      }
      if (c == '`') {
        i++; std::string cmd; while (i < w.size() && w[i] != '`') cmd += w[i++]; if (i < w.size()) i++;
        std::string o = backquote(cmd);
        // command substitution splits on whitespace into separate fields
        bool first = true;
        for (size_t k = 0; k < o.size();) {
          if (o[k] == ' ' || o[k] == '\t' || o[k] == '\n') { k++; continue; }
          if (!first) flush(); first = false;
          while (k < o.size() && o[k] != ' ' && o[k] != '\t' && o[k] != '\n') push_char(o[k++]);
        }
        continue;
      }
      if (c == '\\' && i + 1 < w.size()) { push_char(w[i + 1]); i += 2; continue; }
      if (c == '$') {
        // variable substitution; a list variable produces multiple fields
        std::vector<std::string> parts;
        i = expand_dollar_list(w, i, parts);
        for (size_t k = 0; k < parts.size(); k++) {
          if (k > 0) flush();
          for (char ch : parts[k]) push_char(ch);
        }
        continue;
      }
      if (c == ' ' || c == '\t') { flush(); i++; continue; }
      push_char(c);
      i++;
    }
    flush();

    // filename globbing on fields that contain glob metacharacters
    for (auto &f : fields) {
      if (do_glob && f.find_first_of("*?[") != std::string::npos) {
        auto m = gnash::glob::glob(f, 0);
        if (!m.empty()) { for (auto &g : m) out.push_back(g); continue; }
      }
      out.push_back(f);
    }
  }

  // $-substitution returning a scalar string (for inside "...").
  size_t expand_dollar(const std::string &w, size_t i, std::string &out) {
    std::vector<std::string> parts;
    size_t r = expand_dollar_list(w, i, parts);
    for (size_t k = 0; k < parts.size(); k++) { if (k) out += ' '; out += parts[k]; }
    return r;
  }

  // $-substitution returning a list (each list element a separate field).
  size_t expand_dollar_list(const std::string &w, size_t i, std::vector<std::string> &out) {
    i++;  // past '$'
    if (i >= w.size()) { out.push_back("$"); return i; }
    // ${name} / ${name[..]}
    bool braced = false;
    if (w[i] == '{') { braced = true; i++; }
    // $#name  $?name
    char kind = 0;
    if (w[i] == '#' || w[i] == '?') { kind = w[i]; i++; }
    std::string name;
    if (w[i] == '*') { name = "argv"; i++; }
    else { while (i < w.size() && (std::isalnum((unsigned char)w[i]) || w[i] == '_')) name += w[i++]; }
    std::string index;
    if (i < w.size() && w[i] == '[') { i++; while (i < w.size() && w[i] != ']') index += w[i++]; if (i < w.size()) i++; }
    if (braced && i < w.size() && w[i] == '}') i++;

    if (kind == '#') { auto *v = var_ptr(name); out.push_back(std::to_string(v ? v->size() : (var_set(name) ? 1 : 0))); return i; }
    if (kind == '?') { out.push_back(var_set(name) ? "1" : "0"); return i; }

    auto *v = var_ptr(name);
    std::vector<std::string> vals;
    if (name == "argv") vals = sh.positional;
    else if (v) vals = *v;
    else if (name == "status" || name == "?") vals = {std::to_string(status)};
    else if (name == "0") vals = {sh.arg0};
    else { const char *e = std::getenv(name.c_str()); if (e) vals = {e}; }

    std::vector<std::string> result;
    if (!index.empty()) {
      // resolve numeric index (1-based) or n-m range; expand $ inside index
      std::string idx; { size_t j = 0; while (j < index.size()) { if (index[j] == '$') { std::string s2; j = expand_dollar(index, j, s2); idx += s2; } else idx += index[j++]; } }
      size_t dash = idx.find('-');
      if (dash != std::string::npos) {
        int lo = std::atoi(idx.substr(0, dash).c_str());
        int hi = idx.substr(dash + 1).empty() ? (int)vals.size() : std::atoi(idx.substr(dash + 1).c_str());
        for (int k = lo; k <= hi && k >= 1 && k <= (int)vals.size(); k++) result.push_back(vals[k - 1]);
      } else {
        int k = std::atoi(idx.c_str());
        if (k >= 1 && k <= (int)vals.size()) result.push_back(vals[k - 1]);
      }
    } else {
      result = vals;
      if (vals.empty()) result.push_back("");
    }

    // trailing `:' modifiers (:h head, :t tail, :r root, :e ext, :q quote)
    while (i + 1 < w.size() && w[i] == ':' && std::strchr("htreqxgg", w[i + 1])) {
      bool global = false;
      size_t m = i + 1;
      if (w[m] == 'g') { global = true; m++; }
      if (m >= w.size() || !std::strchr("htre", w[m])) break;
      char mod = w[m];
      for (std::string &s : result) s = apply_modifier(s, mod);
      (void)global;
      i = m + 1;
    }

    for (auto &x : result) out.push_back(x);
    return i;
  }

  static std::string apply_modifier(const std::string &s, char mod) {
    size_t slash = s.rfind('/');
    std::string dir = (slash == std::string::npos) ? "" : s.substr(0, slash);
    std::string base = (slash == std::string::npos) ? s : s.substr(slash + 1);
    size_t dot = base.rfind('.');
    switch (mod) {
      case 't': return base;                                          // tail
      case 'h': return (slash == std::string::npos) ? "." : (dir.empty() ? "/" : dir);  // head
      case 'r': return (dot == std::string::npos || dot == 0) ? s : s.substr(0, s.size() - (base.size() - dot));  // root
      case 'e': return (dot == std::string::npos || dot == 0) ? "" : base.substr(dot + 1);  // extension
      default: return s;
    }
  }

  std::string backquote(const std::string &cmd) {
    int fds[2];
    if (pipe(fds) != 0) return "";
    std::fflush(nullptr);  // don't let the child inherit our buffered stdout
    pid_t pid = fork();
    if (pid == 0) {
      close(fds[0]); dup2(fds[1], 1); close(fds[1]);
      Interp child(sh);
      Parser pr(cmd);
      child.run_block(pr.parse_block({}));
      std::fflush(nullptr);
      _exit(child.status);
    }
    close(fds[1]);
    std::string out; char buf[4096]; ssize_t n;
    while ((n = read(fds[0], buf, sizeof buf)) > 0) out.append(buf, n);
    close(fds[0]);
    int st = 0; waitpid(pid, &st, 0);
    while (!out.empty() && out.back() == '\n') out.pop_back();
    for (char &c : out) if (c == '\n') c = ' ';
    return out;
  }

  // -- expression evaluation --
  struct Expr {
    Interp &in; std::string t; size_t p = 0;
    Expr(Interp &i, const std::string &s) : in(i), t(s) {}
    void ws() { while (p < t.size() && (t[p] == ' ' || t[p] == '\t')) p++; }
    bool eat(const char *op) { ws(); size_t n = std::strlen(op); if (t.compare(p, n, op) == 0) { p += n; return true; } return false; }
    long num(const std::string &s) { return std::strtol(s.c_str(), nullptr, 0); }

    std::string primary() {
      ws();
      if (p < t.size() && t[p] == '(') { p++; std::string v = or_expr(); ws(); if (p < t.size() && t[p] == ')') p++; return v; }
      if (p < t.size() && t[p] == '!') { p++; std::string v = primary(); return num(v) ? "0" : "1"; }
      if (p < t.size() && t[p] == '-' && p + 1 < t.size() && std::strchr("efdrwxozl", t[p + 1]) && (p + 2 >= t.size() || t[p + 2] == ' ')) {
        char op = t[p + 1]; p += 2; ws(); std::string f = word();
        struct stat sb; bool r = false;
        if (op == 'e') r = stat(f.c_str(), &sb) == 0;
        else if (op == 'f') r = stat(f.c_str(), &sb) == 0 && S_ISREG(sb.st_mode);
        else if (op == 'd') r = stat(f.c_str(), &sb) == 0 && S_ISDIR(sb.st_mode);
        else if (op == 'r') r = access(f.c_str(), R_OK) == 0;
        else if (op == 'w') r = access(f.c_str(), W_OK) == 0;
        else if (op == 'x') r = access(f.c_str(), X_OK) == 0;
        else if (op == 'o') r = stat(f.c_str(), &sb) == 0 && sb.st_uid == getuid();
        else if (op == 'z') r = stat(f.c_str(), &sb) == 0 && sb.st_size == 0;
        else if (op == 'l') r = lstat(f.c_str(), &sb) == 0 && S_ISLNK(sb.st_mode);
        return r ? "1" : "0";
      }
      return word();
    }
    std::string word() {
      ws();
      std::string raw;
      while (p < t.size() && !std::strchr(" \t()!&|<>=~", t[p])) raw += t[p++];
      // expand $ and quotes in the operand (no globbing: =~/!~ RHS is a pattern)
      std::vector<std::string> fs; in.expand_word(raw, fs, false);
      std::string r; for (size_t k = 0; k < fs.size(); k++) { if (k) r += ' '; r += fs[k]; }
      return r;
    }
    std::string mul() { std::string v = primary(); for (;;) { ws(); if (eat("*")) v = std::to_string(num(v) * num(primary())); else if (eat("/")) { long d = num(primary()); v = std::to_string(d ? num(v) / d : 0); } else if (eat("%")) { long d = num(primary()); v = std::to_string(d ? num(v) % d : 0); } else break; } return v; }
    std::string add() { std::string v = mul(); for (;;) { ws(); if (p + 1 < t.size() && t[p] == '+' && t[p+1] != '+') { p++; v = std::to_string(num(v) + num(mul())); } else if (t[p] == '-' && (p + 1 >= t.size() || t[p+1] != '-')) { p++; v = std::to_string(num(v) - num(mul())); } else break; } return v; }
    std::string cmp() {
      std::string v = add();
      for (;;) { ws();
        if (eat("<=")) v = num(v) <= num(add()) ? "1" : "0";
        else if (eat(">=")) v = num(v) >= num(add()) ? "1" : "0";
        else if (p < t.size() && t[p] == '<' && (p+1>=t.size()||t[p+1] != '<')) { p++; v = num(v) < num(add()) ? "1" : "0"; }
        else if (p < t.size() && t[p] == '>' && (p+1>=t.size()||t[p+1] != '>')) { p++; v = num(v) > num(add()) ? "1" : "0"; }
        else break;
      }
      return v;
    }
    std::string eq() {
      std::string v = cmp();
      for (;;) { ws();
        if (eat("==")) { std::string r = cmp(); v = (v == r) ? "1" : "0"; }
        else if (eat("!=")) { std::string r = cmp(); v = (v != r) ? "1" : "0"; }
        else if (eat("=~")) { std::string r = cmp(); v = strmatch(const_cast<char*>(r.c_str()), const_cast<char*>(v.c_str()), 0) == 0 ? "1" : "0"; }
        else if (eat("!~")) { std::string r = cmp(); v = strmatch(const_cast<char*>(r.c_str()), const_cast<char*>(v.c_str()), 0) != 0 ? "1" : "0"; }
        else break;
      }
      return v;
    }
    std::string and_expr() { std::string v = eq(); while (true) { ws(); if (eat("&&")) { std::string r = eq(); v = (num(v) && num(r)) ? "1" : "0"; } else break; } return v; }
    std::string or_expr() { std::string v = and_expr(); while (true) { ws(); if (eat("||")) { std::string r = and_expr(); v = (num(v) || num(r)) ? "1" : "0"; } else break; } return v; }
    long eval() { std::string v = or_expr(); return num(v); }
  };
  bool truth(const std::string &raw) { Expr e(*this, raw); return e.eval() != 0; }
  long eval_arith(const std::string &raw) { Expr e(*this, raw); return e.eval(); }

  // -- builtins --
  void set_path_env() {  // keep $PATH in sync with the `path' list
    auto *v = var_ptr("path");
    if (!v) return;
    std::string ps; for (size_t i = 0; i < v->size(); i++) { if (i) ps += ':'; ps += (*v)[i]; }
    setenv("PATH", ps.c_str(), 1);
    sh.set_exported("PATH", ps);
  }

  int do_builtin(const std::vector<std::string> &a) {
    const std::string &c = a[0];
    if (c == "echo") {
      size_t i = 1; bool nl = true;
      if (i < a.size() && a[i] == "-n") { nl = false; i++; }
      for (; i < a.size(); i++) { if (i > 1 + (nl ? 0 : 1)) std::fputc(' ', stdout); std::fputs(a[i].c_str(), stdout); }
      if (nl) std::fputc('\n', stdout);
      return 0;
    }
    if (c == "set") return bi_set(a);
    if (c == "@") return bi_at(a);
    if (c == "setenv") { if (a.size() >= 3) { setenv(a[1].c_str(), a[2].c_str(), 1); sh.set_exported(a[1], a[2]); } else if (a.size() == 2) { setenv(a[1].c_str(), "", 1); sh.set_exported(a[1], ""); } return 0; }
    if (c == "unsetenv") { for (size_t i = 1; i < a.size(); i++) { unsetenv(a[i].c_str()); sh.unset(a[i]); } return 0; }
    if (c == "unset") { for (size_t i = 1; i < a.size(); i++) sh.csh_vars.erase(a[i]); return 0; }
    if (c == "cd" || c == "chdir") { std::string d = a.size() > 1 ? a[1] : var_str("home"); if (chdir(d.c_str()) != 0) { std::fprintf(stderr, "%s: Can't change to \"%s\".\n", sh.shell_name.c_str(), d.c_str()); return 1; } char b[4096]; if (getcwd(b, sizeof b)) sh.csh_vars["cwd"] = {b}; return 0; }
    if (c == "exit") { exiting = true; exit_code = a.size() > 1 ? (int)eval_arith(a[1]) : status; return exit_code; }
    if (c == "source") { if (a.size() > 1) { std::FILE *f = std::fopen(a[1].c_str(), "r"); if (f) { std::string body; char b[4096]; size_t n; while ((n = std::fread(b, 1, sizeof b, f)) > 0) body.append(b, n); std::fclose(f); Parser pr(body); run_block(pr.parse_block({})); } } return 0; }
    if (c == "alias" || c == "unalias" || c == "rehash" || c == "unhash" || c == "hashstat" ||
        c == "limit" || c == "unlimit" || c == "nice" || c == "onintr" || c == "notify" ||
        c == "history" || c == "bindkey" || c == "complete" || c == "uncomplete" || c == "umask" ||
        c == "stty" || c == "ttyctl" || c == "sched" || c == "settc" || c == "setty")
      return 0;  // accepted, not yet acted on
    return -1;  // not a builtin
  }

  int bi_set(const std::vector<std::string> &a) {
    if (a.size() == 1) {  // list all
      std::vector<std::string> names;
      for (auto &kv : sh.csh_vars) names.push_back(kv.first);
      std::sort(names.begin(), names.end());
      for (auto &n : names) {
        auto &v = sh.csh_vars[n];
        if (v.size() == 1) std::printf("%s\t%s\n", n.c_str(), v[0].c_str());
        else { std::printf("%s\t(", n.c_str()); for (size_t i = 0; i < v.size(); i++) { if (i) std::putchar(' '); std::fputs(v[i].c_str(), stdout); } std::printf(")\n"); }
      }
      return 0;
    }
    // parse: name = value | name = (list) | name | name[i] = value  (words already expanded)
    size_t i = 1;
    while (i < a.size()) {
      std::string name = a[i++];
      // handle name=value glued
      std::string glued;
      auto eq = name.find('=');
      std::string idx;
      auto br = name.find('[');
      if (br != std::string::npos && name.find(']') != std::string::npos) { idx = name.substr(br + 1, name.find(']') - br - 1); name = name.substr(0, br); }
      if (eq != std::string::npos) { glued = name.substr(eq + 1); name = name.substr(0, eq); }
      std::vector<std::string> vals;
      bool assigned = false;
      if (!glued.empty()) { vals = {glued}; assigned = true; }
      else if (i < a.size() && a[i] == "=") {
        i++;
        if (i < a.size() && a[i] == "(") { i++; while (i < a.size() && a[i] != ")") vals.push_back(a[i++]); if (i < a.size()) i++; }
        else {
          // Grab the value word(s).  A single backquote/variable that expanded
          // to several fields becomes a list; stop before a following `name ='
          // (a second assignment in the same `set').
          while (i < a.size() && a[i] != "(" && a[i] != ")" &&
                 !(i + 1 < a.size() && a[i + 1] == "=")) vals.push_back(a[i++]);
        }
        assigned = true;
      }
      if (!idx.empty()) { int k = std::atoi(idx.c_str()); auto &v = sh.csh_vars[name]; if (k >= 1) { if ((int)v.size() < k) v.resize(k); v[k - 1] = vals.empty() ? "" : vals[0]; } }
      else if (assigned) sh.csh_vars[name] = vals;
      else sh.csh_vars[name] = {""};
      if (name == "path") set_path_env();
    }
    return 0;
  }

  int bi_at(const std::vector<std::string> &a) {
    if (a.size() < 2) return 0;
    std::string name = a[1];
    std::string idx;
    auto br = name.find('[');
    if (br != std::string::npos) { idx = name.substr(br + 1, name.find(']') - br - 1); name = name.substr(0, br); }
    // @ name++ / name--
    if (a.size() == 2 && (name.size() > 2 && (name.substr(name.size() - 2) == "++" || name.substr(name.size() - 2) == "--"))) {
      bool inc = name.substr(name.size() - 2) == "++"; name = name.substr(0, name.size() - 2);
      long v = std::atol(var_str(name).c_str()) + (inc ? 1 : -1);
      sh.csh_vars[name] = {std::to_string(v)}; return 0;
    }
    // @ name op expr   (op is = or += etc.)
    size_t i = 2; std::string op = i < a.size() ? a[i++] : "=";
    std::string exprtext; for (; i < a.size(); i++) { if (!exprtext.empty()) exprtext += ' '; exprtext += a[i]; }
    long r = eval_arith(exprtext);
    long cur = std::atol(var_str(name).c_str());
    if (op == "+=") r = cur + r; else if (op == "-=") r = cur - r;
    else if (op == "*=") r = cur * r; else if (op == "/=") r = r ? cur / r : 0; else if (op == "%=") r = r ? cur % r : 0;
    if (!idx.empty()) { int k = std::atoi(idx.c_str()); auto &v = sh.csh_vars[name]; if (k >= 1) { if ((int)v.size() < k) v.resize(k); v[k - 1] = std::to_string(r); } }
    else sh.csh_vars[name] = {std::to_string(r)};
    return 0;
  }

  // -- command execution --
  static bool is_builtin(const std::string &c) {
    static const std::set<std::string> b = {
        "echo", "set", "@", "setenv", "unsetenv", "unset", "cd", "chdir", "exit",
        "source", "alias", "unalias", "rehash", "unhash", "hashstat", "limit",
        "unlimit", "nice", "onintr", "notify", "history", "bindkey", "complete",
        "uncomplete", "umask", "stty", "ttyctl", "sched", "settc", "setty"};
    return b.count(c) != 0;
  }
  void apply_redirs(const std::vector<Redir> &rs) {
    for (const Redir &r : rs) {
      if (r.op == '<') { int f = open(r.target.c_str(), O_RDONLY); if (f >= 0) { dup2(f, 0); close(f); } }
      else { int fl = O_WRONLY | O_CREAT | (r.op == 'A' ? O_APPEND : O_TRUNC); int f = open(r.target.c_str(), fl, 0666); if (f >= 0) { dup2(f, 1); if (r.op == 'E') dup2(f, 2); close(f); } }
    }
  }

  int run_pipeline(const Pipeline &pl) {
    // expand each stage's words + redirs
    std::vector<std::vector<std::string>> argvs;
    std::vector<std::vector<Redir>> redirs;
    for (const Cmd &cm : pl.cmds) {
      std::vector<std::string> argv;
      // `@' arguments are an arithmetic expression: `*', `?' etc. are operators,
      // not filename globs, so its words are expanded without globbing.
      bool do_glob = cm.words.empty() || cm.words[0] != "@";
      for (const std::string &w : cm.words) expand_word(w, argv, do_glob);
      std::vector<Redir> rs;
      for (const Redir &r : cm.redirs) { std::vector<std::string> t; expand_word(r.target, t); rs.push_back({r.op, t.empty() ? "" : t[0]}); }
      argvs.push_back(argv);
      redirs.push_back(rs);
    }
    if (argvs.size() == 1 && !argvs[0].empty() && !pl.background && is_builtin(argvs[0][0])) {
      // single builtin: run in-process, applying any redirections around it
      int si = dup(0), so = dup(1), se = dup(2);
      apply_redirs(redirs[0]);
      int b = do_builtin(argvs[0]);
      std::fflush(nullptr);
      dup2(si, 0); dup2(so, 1); dup2(se, 2);
      close(si); close(so); close(se);
      status = (b == -1) ? 0 : b;
      return status;
    }
    // external (and multi-stage) commands via fork/exec
    size_t n = argvs.size();
    int prev = -1; std::vector<pid_t> pids;
    std::fflush(nullptr);  // flush buffered stdout so children don't duplicate it
    for (size_t k = 0; k < n; k++) {
      int fds[2] = {-1, -1};
      if (k + 1 < n) { if (pipe(fds) != 0) break; }
      pid_t pid = fork();
      if (pid == 0) {
        if (prev != -1) { dup2(prev, 0); close(prev); }
        if (k + 1 < n) { close(fds[0]); dup2(fds[1], 1); close(fds[1]); }
        for (const Redir &r : redirs[k]) {
          if (r.op == '<') { int f = open(r.target.c_str(), O_RDONLY); if (f >= 0) { dup2(f, 0); close(f); } }
          else { int fl = O_WRONLY | O_CREAT | (r.op == 'A' ? O_APPEND : O_TRUNC); int f = open(r.target.c_str(), fl, 0666); if (f >= 0) { dup2(f, 1); if (r.op == 'E') dup2(f, 2); close(f); } }
        }
        if (argvs[k].empty()) _exit(0);
        // builtins in a pipeline stage
        int b = do_builtin(argvs[k]);
        if (b != -1) { std::fflush(nullptr); _exit(b); }
        std::vector<std::string> env = sh.environ_block();
        std::vector<char *> ep; for (auto &e : env) ep.push_back(const_cast<char *>(e.c_str())); ep.push_back(nullptr);
        environ = ep.data();
        std::vector<char *> cv; for (auto &w : argvs[k]) cv.push_back(const_cast<char *>(w.c_str())); cv.push_back(nullptr);
        execvp(cv[0], cv.data());
        std::fprintf(stderr, "%s: Command not found.\n", argvs[k][0].c_str());
        _exit(1);
      }
      pids.push_back(pid);
      if (prev != -1) close(prev);
      if (k + 1 < n) { close(fds[1]); prev = fds[0]; }
    }
    if (pl.background) { if (!pids.empty()) sh.last_bg_pid = pids.back(); return 0; }
    int st = 0;
    for (pid_t pid : pids) { int w = 0; waitpid(pid, &w, 0); st = WIFEXITED(w) ? WEXITSTATUS(w) : 128 + (WIFSIGNALED(w) ? WTERMSIG(w) : 0); }
    status = st;
    return st;
  }

  void run_stmt(const Stmt &st) {
    if (exiting || loop_ctrl) return;
    switch (st.kind) {
      case Kind::Simple: run_pipeline(st.pipe); break;
      case Kind::If:
        if (truth(st.expr)) run_block(st.then_body);
        else run_block(st.else_body);
        break;
      case Kind::Foreach: {
        std::vector<std::string> items;
        for (const std::string &w : st.list) expand_word(w, items);
        for (const std::string &it : items) {
          sh.csh_vars[st.var] = {it};
          run_block(st.body);
          if (loop_ctrl == 2) loop_ctrl = 0;       // continue
          if (loop_ctrl == 1) { loop_ctrl = 0; break; }  // break
          if (exiting) break;
        }
        break;
      }
      case Kind::While: {
        int guard = 0;
        while (truth(st.expr) && guard++ < 1000000) {
          run_block(st.body);
          if (loop_ctrl == 2) loop_ctrl = 0;
          if (loop_ctrl == 1) { loop_ctrl = 0; break; }
          if (exiting) break;
        }
        break;
      }
      case Kind::Switch: {
        std::vector<std::string> ws; expand_word(st.switch_word, ws, false);
        std::string subject = ws.empty() ? "" : ws[0];
        int start = -1;
        for (size_t k = 0; k < st.arms.size(); k++) {
          std::vector<std::string> ps; if (!st.arms[k].is_default) expand_word(st.arms[k].pattern, ps, false);
          std::string pat = ps.empty() ? st.arms[k].pattern : ps[0];
          if (st.arms[k].is_default) { if (start < 0) start = (int)k; }
          else if (strmatch(const_cast<char*>(pat.c_str()), const_cast<char*>(subject.c_str()), 0) == 0) { start = (int)k; break; }
        }
        if (start >= 0) {  // fall through arms until breaksw
          for (size_t k = start; k < st.arms.size(); k++) {
            run_block(st.arms[k].body);
            if (loop_ctrl == 3) { loop_ctrl = 0; break; }
            if (loop_ctrl || exiting) break;
          }
        }
        break;
      }
      case Kind::Break: loop_ctrl = 1; break;
      case Kind::Continue: loop_ctrl = 2; break;
      case Kind::Breaksw: loop_ctrl = 3; break;
    }
  }

  void run_block(const std::vector<StmtPtr> &b) {
    for (const auto &st : b) { if (exiting || loop_ctrl) break; run_stmt(*st); }
  }
};

void csh_init(Shell &sh) {
  if (sh.csh_inited) return;
  sh.csh_inited = true;
  char b[4096];
  if (getcwd(b, sizeof b)) sh.csh_vars["cwd"] = {b};
  const char *home = std::getenv("HOME"); if (home) sh.csh_vars["home"] = {home};
  const char *user = std::getenv("USER"); if (user) sh.csh_vars["user"] = {user};
  const char *term = std::getenv("TERM"); if (term) sh.csh_vars["term"] = {term};
  const char *path = std::getenv("PATH");
  if (path) { std::vector<std::string> ps; std::string p = path; size_t i = 0; while (i <= p.size()) { size_t j = p.find(':', i); std::string d = p.substr(i, j == std::string::npos ? std::string::npos : j - i); ps.push_back(d.empty() ? "." : d); if (j == std::string::npos) break; i = j + 1; } sh.csh_vars["path"] = ps; }
  sh.csh_vars["shell"] = {sh.get("SHELL").empty() ? "/bin/csh" : sh.get("SHELL")};
  sh.csh_vars["version"] = {"tcsh 6.21.00 (gnash) options"};
  sh.csh_vars["argv"] = sh.positional;
  sh.csh_vars["GNASH_PERSONALITY"] = {sh.personality_name};  // observable persona
  if (getuid() == 0) sh.csh_vars["prompt"] = {"# "}; else sh.csh_vars["prompt"] = {"> "};
}

}  // namespace

int run_csh(Shell &sh, const std::string &script) {
  csh_init(sh);
  Interp in(sh);
  Parser pr(script);
  std::vector<StmtPtr> prog = pr.parse_block({});
  in.run_block(prog);
  if (in.exiting) { sh.exiting = true; sh.exit_status = in.exit_code; }
  sh.last_status = in.status;
  return in.status;
}

}  // namespace gnash::core
