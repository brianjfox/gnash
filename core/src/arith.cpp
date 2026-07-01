// arith.cpp -- shell arithmetic evaluation (a recursive-descent evaluator).
//
// Supports the C-like operator set bash exposes: assignment (= += -= ...),
// ternary ?:, || && | ^ & == != < <= > >= << >> + - * / % **, unary + - ! ~,
// pre/post ++ and --, parentheses, comma, and variable references (recursively
// evaluated, like bash).  Bases: decimal, 0x hex, leading-0 octal.

#include <cctype>
#include <cstdlib>
#include <string>

#include "gnash/core/shell.hpp"

namespace gnash::core {

namespace {

struct Eval {
  Shell &sh;
  const std::string &s;
  size_t pos = 0;
  bool ok = true;
  int depth;

  Eval(Shell &shell, const std::string &str, int d) : sh(shell), s(str), depth(d) {}

  void skip() {
    while (pos < s.size() && std::isspace(static_cast<unsigned char>(s[pos]))) pos++;
  }
  char peek() {
    skip();
    return pos < s.size() ? s[pos] : '\0';
  }
  bool eat(const char *op) {
    skip();
    size_t n = std::strlen(op);
    if (s.compare(pos, n, op) == 0) {
      // avoid matching a prefix of a longer operator (e.g. `<` in `<<`)
      pos += n;
      return true;
    }
    return false;
  }

  long long var_value(const std::string &name) {
    std::string v = sh.get(name);
    if (v.empty()) { std::string dv; if (sh.dynamic_var(name, dv)) v = dv; }
    if (v.empty()) return 0;
    if (depth > 100) return 0;
    bool sub_ok = true;
    long long r = eval_expr_string(sh, v, depth + 1, &sub_ok);
    return sub_ok ? r : 0;
  }

  static long long eval_expr_string(Shell &sh, const std::string &str, int d, bool *ok) {
    Eval e(sh, str, d);
    long long v = e.comma();
    e.skip();
    if (e.pos != str.size()) e.ok = false;
    if (ok) *ok = e.ok;
    return v;
  }

  std::string read_name() {
    skip();
    size_t start = pos;
    if (pos < s.size() && (std::isalpha(static_cast<unsigned char>(s[pos])) || s[pos] == '_')) {
      pos++;
      while (pos < s.size() &&
             (std::isalnum(static_cast<unsigned char>(s[pos])) || s[pos] == '_'))
        pos++;
      return s.substr(start, pos - start);
    }
    return std::string();
  }

  // comma
  long long comma() {
    long long v = assignment();
    while (peek() == ',') {
      pos++;
      v = assignment();
    }
    return v;
  }

  long long assignment() {
    size_t save = pos;
    std::string name = read_name();
    if (!name.empty()) {
      skip();
      static const char *ops[] = {"=",  "+=", "-=", "*=", "/=", "%=",
                                  "<<=", ">>=", "&=", "^=", "|=", nullptr};
      // Match the longest assignment operator, but not == (equality).
      for (int i = 0; ops[i]; i++) {
        size_t n = std::strlen(ops[i]);
        if (s.compare(pos, n, ops[i]) == 0 &&
            !(ops[i][0] == '=' && pos + 1 < s.size() && s[pos + 1] == '=')) {
          pos += n;
          long long rhs = assignment();
          long long cur = var_value(name);
          long long res = rhs;
          std::string o = ops[i];
          if (o == "+=") res = cur + rhs;
          else if (o == "-=") res = cur - rhs;
          else if (o == "*=") res = cur * rhs;
          else if (o == "/=") { if (rhs == 0) { ok = false; res = 0; } else res = cur / rhs; }
          else if (o == "%=") { if (rhs == 0) { ok = false; res = 0; } else res = cur % rhs; }
          else if (o == "<<=") res = cur << rhs;
          else if (o == ">>=") res = cur >> rhs;
          else if (o == "&=") res = cur & rhs;
          else if (o == "^=") res = cur ^ rhs;
          else if (o == "|=") res = cur | rhs;
          sh.set(name, std::to_string(res));
          return res;
        }
      }
    }
    pos = save;
    return ternary();
  }

  long long ternary() {
    long long c = logic_or();
    if (peek() == '?') {
      pos++;
      long long a = assignment();
      skip();
      if (peek() == ':') pos++; else ok = false;
      long long b = ternary();
      return c ? a : b;
    }
    return c;
  }

  long long logic_or() {
    long long v = logic_and();
    // NB: always parse the RHS (don't let C++ short-circuit skip consuming it).
    while (eat("||")) { long long r = logic_and(); v = (v || r) ? 1 : 0; }
    return v;
  }
  long long logic_and() {
    long long v = bit_or();
    while (eat("&&")) { long long r = bit_or(); v = (v && r) ? 1 : 0; }
    return v;
  }
  long long bit_or() {
    long long v = bit_xor();
    for (;;) {
      skip();
      if (peek() == '|' && s.compare(pos, 2, "||") != 0) { pos++; v |= bit_xor(); }
      else break;
    }
    return v;
  }
  long long bit_xor() {
    long long v = bit_and();
    while (peek() == '^') { pos++; v ^= bit_and(); }
    return v;
  }
  long long bit_and() {
    long long v = equality();
    for (;;) {
      skip();
      if (peek() == '&' && s.compare(pos, 2, "&&") != 0) { pos++; v &= equality(); }
      else break;
    }
    return v;
  }
  long long equality() {
    long long v = relational();
    for (;;) {
      if (eat("==")) v = (v == relational()) ? 1 : 0;
      else if (eat("!=")) v = (v != relational()) ? 1 : 0;
      else break;
    }
    return v;
  }
  long long relational() {
    long long v = shift();
    for (;;) {
      if (eat("<=")) v = (v <= shift()) ? 1 : 0;
      else if (eat(">=")) v = (v >= shift()) ? 1 : 0;
      else if (eat("<") && peek() != '<') v = (v < shift()) ? 1 : 0;
      else if (eat(">") && peek() != '>') v = (v > shift()) ? 1 : 0;
      else break;
    }
    return v;
  }
  long long shift() {
    long long v = additive();
    for (;;) {
      if (eat("<<")) v <<= additive();
      else if (eat(">>")) v >>= additive();
      else break;
    }
    return v;
  }
  long long additive() {
    long long v = multiplicative();
    for (;;) {
      skip();
      char c = peek();
      if (c == '+' && s.compare(pos, 2, "++") != 0) { pos++; v += multiplicative(); }
      else if (c == '-' && s.compare(pos, 2, "--") != 0) { pos++; v -= multiplicative(); }
      else break;
    }
    return v;
  }
  long long multiplicative() {
    long long v = power();
    for (;;) {
      skip();
      char c = peek();
      if (c == '*' && s.compare(pos, 2, "**") != 0) { pos++; v *= power(); }
      else if (c == '/') { pos++; long long d = power(); if (d == 0) { ok = false; } else v /= d; }
      else if (c == '%') { pos++; long long d = power(); if (d == 0) { ok = false; } else v %= d; }
      else break;
    }
    return v;
  }
  long long power() {
    long long base = unary();
    if (eat("**")) {
      long long e = power();
      long long r = 1;
      for (long long k = 0; k < e; k++) r *= base;
      return r;
    }
    return base;
  }
  long long unary() {
    skip();
    if (eat("++")) { return preincr(1); }
    if (eat("--")) { return preincr(-1); }
    char c = peek();
    if (c == '+') { pos++; return unary(); }
    if (c == '-') { pos++; return -unary(); }
    if (c == '!') { pos++; return unary() ? 0 : 1; }
    if (c == '~') { pos++; return ~unary(); }
    return postfix();
  }
  long long preincr(int delta) {
    std::string name = read_name();
    if (name.empty()) { ok = false; return 0; }
    long long v = var_value(name) + delta;
    sh.set(name, std::to_string(v));
    return v;
  }
  long long postfix() {
    size_t save = pos;
    std::string name = read_name();
    if (!name.empty()) {
      skip();
      if (s.compare(pos, 2, "++") == 0) { pos += 2; long long v = var_value(name); sh.set(name, std::to_string(v + 1)); return v; }
      if (s.compare(pos, 2, "--") == 0) { pos += 2; long long v = var_value(name); sh.set(name, std::to_string(v - 1)); return v; }
      pos = save;  // not a postfix; fall through to primary (which re-reads name)
    } else {
      pos = save;
    }
    return primary();
  }
  long long primary() {
    skip();
    if (peek() == '(') {
      pos++;
      long long v = comma();
      skip();
      if (peek() == ')') pos++; else ok = false;
      return v;
    }
    // number
    if (pos < s.size() && std::isdigit(static_cast<unsigned char>(s[pos]))) {
      char *end = nullptr;
      long long v = std::strtoll(s.c_str() + pos, &end, 0);  // 0x/0 prefixes honored
      pos = static_cast<size_t>(end - s.c_str());
      return v;
    }
    // variable
    std::string name = read_name();
    if (!name.empty()) return var_value(name);
    ok = false;
    return 0;
  }
};

}  // namespace

long long eval_arith(Shell &sh, const std::string &expr, bool *ok) {
  Eval e(sh, expr, 0);
  long long v = e.comma();
  e.skip();
  if (e.pos != expr.size()) e.ok = false;
  if (ok) *ok = e.ok;
  return v;
}

}  // namespace gnash::core
