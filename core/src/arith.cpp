// Copyright (c) 2026 Brian J. Fox
// Licensed under GPLv2 with the GPLv2-AI Exception.

// arith.cpp -- shell arithmetic evaluation.
//
// Supports the C-like operator set bash exposes: assignment (= += -= ...),
// ternary ?:, || && | ^ & == != < <= > >= << >> + - * / % **, unary + - ! ~,
// pre/post ++ and --, parentheses, comma, and variable references (recursively
// evaluated, like bash).  Bases: decimal, 0x hex, leading-0 octal.
//
// Parsing and evaluation are separated: an expression string is parsed once
// into an AST and cached, so re-evaluating the same expression (e.g. a
// `for ((;;))' condition/update every iteration) skips lexing and parsing.

#include <cctype>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <string>

#include "gnash/core/shell.hpp"

namespace gnash::core {

namespace {

enum class K {
  Num, Var,
  Neg, LNot, BNot,
  PreInc, PreDec, PostInc, PostDec,
  Mul, Div, Mod, Pow, Add, Sub, Shl, Shr,
  Lt, Le, Gt, Ge, Eq, Ne, BAnd, BXor, BOr, LAnd, LOr,
  Ternary, Comma, Assign
};

struct Node;
using NodeP = std::unique_ptr<Node>;

struct Node {
  K k;
  long long num = 0;          // Num
  std::string name;           // Var / lvalue name (Var, Pre/Post Inc/Dec, Assign)
  bool has_sub = false;       // name has a [subscript]
  std::string sub;            // raw subscript text (evaluated by array_get/set)
  const char *aop = nullptr;  // Assign operator ("=", "+=", ...)
  NodeP a, b, c;              // operands
};

NodeP mk(K k) { auto n = std::make_unique<Node>(); n->k = k; return n; }

// Digit value of C in the given BASE for bash's `base#digits' notation, or -1 if
// C is not a digit in that base.  Digits run 0-9, then a-z (10-35); for a base
// above 36, A-Z are 36-61 and `@'/`_' are 62/63, while for base <= 36 upper- and
// lowercase letters are interchangeable (10-35), matching bash.
int base_digit(char c, long long base) {
  int d;
  if (c >= '0' && c <= '9') d = c - '0';
  else if (base <= 36) {
    if (c >= 'a' && c <= 'z') d = c - 'a' + 10;
    else if (c >= 'A' && c <= 'Z') d = c - 'A' + 10;
    else return -1;
  } else {
    if (c >= 'a' && c <= 'z') d = c - 'a' + 10;
    else if (c >= 'A' && c <= 'Z') d = c - 'A' + 36;
    else if (c == '@') d = 62;
    else if (c == '_') d = 63;
    else return -1;
  }
  return d < base ? d : -1;
}

// ---- parser: string -> AST (no shell access; pure) ------------------------

struct Parser {
  const std::string &s;
  size_t pos = 0;
  bool ok = true;
  explicit Parser(const std::string &str) : s(str) {}

  // Bound recursion so a deeply nested expression -- e.g. thousands of nested
  // parentheses or unary operators in $(( ... )) -- fails to parse instead of
  // overflowing the call stack and crashing the shell.  Far above any real use.
  int rec_depth = 0;
  static constexpr int kMaxDepth = 1000;
  struct DepthGuard {
    Parser &p;
    bool allowed;
    explicit DepthGuard(Parser &pp) : p(pp) {
      allowed = (++p.rec_depth <= kMaxDepth);
      if (!allowed) p.ok = false;
    }
    ~DepthGuard() { --p.rec_depth; }
  };

  void skip() { while (pos < s.size() && std::isspace(static_cast<unsigned char>(s[pos]))) pos++; }
  char peek() { skip(); return pos < s.size() ? s[pos] : '\0'; }
  bool eat(const char *op) {
    skip();
    size_t n = std::strlen(op);
    if (s.compare(pos, n, op) == 0) { pos += n; return true; }
    return false;
  }

  std::string read_name() {
    skip();
    size_t start = pos;
    if (pos < s.size() && (std::isalpha(static_cast<unsigned char>(s[pos])) || s[pos] == '_')) {
      pos++;
      while (pos < s.size() && (std::isalnum(static_cast<unsigned char>(s[pos])) || s[pos] == '_')) pos++;
      return s.substr(start, pos - start);
    }
    return std::string();
  }

  // NAME or NAME[subscript]; fills the lvalue fields of `n'.
  bool read_ref(Node &n) {
    n.name = read_name();
    if (n.name.empty()) return false;
    skip();
    if (pos < s.size() && s[pos] == '[') {
      pos++;
      int bdepth = 1;
      while (pos < s.size() && bdepth > 0) {
        if (s[pos] == '[') bdepth++;
        else if (s[pos] == ']') { if (--bdepth == 0) { pos++; break; } }
        if (bdepth > 0) n.sub += s[pos];
        pos++;
      }
      n.has_sub = true;
    }
    return true;
  }

  NodeP binary(K k, NodeP a, NodeP b) {
    auto n = mk(k); n->a = std::move(a); n->b = std::move(b); return n;
  }

  NodeP comma() {
    NodeP v = assignment();
    while (peek() == ',') { pos++; v = binary(K::Comma, std::move(v), assignment()); }
    return v;
  }

  NodeP assignment() {
    size_t save = pos;
    auto lv = mk(K::Assign);
    if (read_ref(*lv)) {
      skip();
      static const char *ops[] = {"=",  "+=", "-=", "*=", "/=", "%=",
                                  "<<=", ">>=", "&=", "^=", "|=", nullptr};
      for (int i = 0; ops[i]; i++) {
        size_t n = std::strlen(ops[i]);
        if (s.compare(pos, n, ops[i]) == 0 &&
            !(ops[i][0] == '=' && pos + 1 < s.size() && s[pos + 1] == '=')) {
          pos += n;
          lv->aop = ops[i];
          lv->a = assignment();  // right-associative
          return lv;
        }
      }
    }
    pos = save;
    return ternary();
  }

  NodeP ternary() {
    NodeP c = logic_or();
    if (peek() == '?') {
      pos++;
      auto n = mk(K::Ternary);
      n->a = std::move(c);
      n->b = assignment();
      skip();
      if (peek() == ':') pos++; else ok = false;
      n->c = ternary();
      return n;
    }
    return c;
  }

  NodeP logic_or() {
    NodeP v = logic_and();
    while (eat("||")) v = binary(K::LOr, std::move(v), logic_and());
    return v;
  }
  NodeP logic_and() {
    NodeP v = bit_or();
    while (eat("&&")) v = binary(K::LAnd, std::move(v), bit_or());
    return v;
  }
  NodeP bit_or() {
    NodeP v = bit_xor();
    for (;;) { skip();
      if (peek() == '|' && s.compare(pos, 2, "||") != 0) { pos++; v = binary(K::BOr, std::move(v), bit_xor()); }
      else break;
    }
    return v;
  }
  NodeP bit_xor() {
    NodeP v = bit_and();
    while (peek() == '^') { pos++; v = binary(K::BXor, std::move(v), bit_and()); }
    return v;
  }
  NodeP bit_and() {
    NodeP v = equality();
    for (;;) { skip();
      if (peek() == '&' && s.compare(pos, 2, "&&") != 0) { pos++; v = binary(K::BAnd, std::move(v), equality()); }
      else break;
    }
    return v;
  }
  NodeP equality() {
    NodeP v = relational();
    for (;;) {
      if (eat("==")) v = binary(K::Eq, std::move(v), relational());
      else if (eat("!=")) v = binary(K::Ne, std::move(v), relational());
      else break;
    }
    return v;
  }
  NodeP relational() {
    NodeP v = shift();
    for (;;) {
      if (eat("<=")) v = binary(K::Le, std::move(v), shift());
      else if (eat(">=")) v = binary(K::Ge, std::move(v), shift());
      else if (eat("<") && peek() != '<') v = binary(K::Lt, std::move(v), shift());
      else if (eat(">") && peek() != '>') v = binary(K::Gt, std::move(v), shift());
      else break;
    }
    return v;
  }
  NodeP shift() {
    NodeP v = additive();
    for (;;) {
      if (eat("<<")) v = binary(K::Shl, std::move(v), additive());
      else if (eat(">>")) v = binary(K::Shr, std::move(v), additive());
      else break;
    }
    return v;
  }
  NodeP additive() {
    NodeP v = multiplicative();
    for (;;) { skip(); char c = peek();
      if (c == '+' && s.compare(pos, 2, "++") != 0) { pos++; v = binary(K::Add, std::move(v), multiplicative()); }
      else if (c == '-' && s.compare(pos, 2, "--") != 0) { pos++; v = binary(K::Sub, std::move(v), multiplicative()); }
      else break;
    }
    return v;
  }
  NodeP multiplicative() {
    NodeP v = power();
    for (;;) { skip(); char c = peek();
      if (c == '*' && s.compare(pos, 2, "**") != 0) { pos++; v = binary(K::Mul, std::move(v), power()); }
      else if (c == '/') { pos++; v = binary(K::Div, std::move(v), power()); }
      else if (c == '%') { pos++; v = binary(K::Mod, std::move(v), power()); }
      else break;
    }
    return v;
  }
  NodeP power() {
    NodeP base = unary();
    if (eat("**")) return binary(K::Pow, std::move(base), power());  // right-associative
    return base;
  }
  NodeP unary() {
    DepthGuard g(*this);
    if (!g.allowed) return mk(K::Num);  // too deep: bail with a placeholder
    skip();
    if (eat("++")) return preincr(K::PreInc);
    if (eat("--")) return preincr(K::PreDec);
    char c = peek();
    if (c == '+') { pos++; return unary(); }
    if (c == '-') { pos++; auto n = mk(K::Neg); n->a = unary(); return n; }
    if (c == '!') { pos++; auto n = mk(K::LNot); n->a = unary(); return n; }
    if (c == '~') { pos++; auto n = mk(K::BNot); n->a = unary(); return n; }
    return postfix();
  }
  NodeP preincr(K k) {
    auto n = mk(k);
    if (!read_ref(*n)) { ok = false; }
    return n;
  }
  NodeP postfix() {
    size_t save = pos;
    auto ref = mk(K::Var);
    if (read_ref(*ref)) {
      skip();
      if (s.compare(pos, 2, "++") == 0) { pos += 2; ref->k = K::PostInc; return ref; }
      if (s.compare(pos, 2, "--") == 0) { pos += 2; ref->k = K::PostDec; return ref; }
      pos = save;  // not a postfix; re-read as a primary
    } else {
      pos = save;
    }
    return primary();
  }
  NodeP primary() {
    skip();
    if (peek() == '(') {
      pos++;
      NodeP v = comma();
      skip();
      if (peek() == ')') pos++; else ok = false;
      return v;
    }
    if (pos < s.size() && std::isdigit(static_cast<unsigned char>(s[pos]))) {
      // bash `base#digits': a decimal base (2..64) followed by '#' and digits in
      // that base.  Detected by looking past the leading run of decimal digits.
      size_t p = pos;
      while (p < s.size() && std::isdigit(static_cast<unsigned char>(s[p]))) p++;
      if (p < s.size() && s[p] == '#') {
        long long base = std::strtoll(s.substr(pos, p - pos).c_str(), nullptr, 10);
        size_t q = p + 1;
        long long v = 0;
        bool any = false;
        for (; q < s.size(); q++) {
          int d = base_digit(s[q], base);
          if (d < 0) break;
          v = v * base + d;
          any = true;
        }
        if (base >= 2 && base <= 64 && any) {
          pos = q;
          auto n = mk(K::Num); n->num = v; return n;
        }
        // malformed base spec: fall through to the ordinary integer parse
      }
      char *end = nullptr;
      long long v = std::strtoll(s.c_str() + pos, &end, 0);  // 0x/0 prefixes honored
      pos = static_cast<size_t>(end - s.c_str());
      auto n = mk(K::Num); n->num = v; return n;
    }
    auto ref = mk(K::Var);
    if (read_ref(*ref)) return ref;
    ok = false;
    return mk(K::Num);  // placeholder 0
  }
};

// A parsed expression plus whether it parsed cleanly (a malformed expression
// evaluates to a value but reports failure, matching the old behavior).  The
// AST is held by shared_ptr and returned BY VALUE, so a caller keeps it alive
// even if a nested evaluation clears the cache mid-walk.
struct Parsed { std::shared_ptr<Node> root; bool ok; };

Parsed parse_cached(const std::string &expr) {
  static std::map<std::string, Parsed> cache;
  auto it = cache.find(expr);
  if (it != cache.end()) return it->second;  // copies the shared_ptr
  if (cache.size() > 4096) cache.clear();     // bound memory (callers hold their own ref)
  Parser p(expr);
  std::shared_ptr<Node> root = p.comma();
  p.skip();
  Parsed parsed{std::move(root), p.ok && p.pos == expr.size()};
  cache.emplace(expr, parsed);
  return parsed;
}

// A plain integer literal (decimal / 0x / 0-octal), the common form of a
// variable's value -- parsed directly so it never enters the cache.
bool try_int(const std::string &s, long long &out) {
  size_t i = 0, n = s.size();
  while (i < n && std::isspace(static_cast<unsigned char>(s[i]))) i++;
  size_t start = i;
  if (i < n && (s[i] == '+' || s[i] == '-')) i++;
  if (i >= n || !std::isdigit(static_cast<unsigned char>(s[i]))) return false;
  char *end = nullptr;
  out = std::strtoll(s.c_str() + start, &end, 0);
  size_t j = static_cast<size_t>(end - s.c_str());
  while (j < n && std::isspace(static_cast<unsigned char>(s[j]))) j++;
  return j == n;
}

// ---- evaluation over the AST ----------------------------------------------

struct Ctx { Shell &sh; bool ok = true; int depth = 0; };

long long eval_node(const Node *n, Ctx &ctx);  // fwd

long long eval_string(Shell &sh, const std::string &str, int depth, bool *ok) {
  long long iv;
  if (try_int(str, iv)) { if (ok) *ok = true; return iv; }
  Parsed p = parse_cached(str);
  Ctx ctx{sh, p.ok, depth};
  long long v = eval_node(p.root.get(), ctx);
  if (ok) *ok = ctx.ok;
  return v;
}

// Read a variable/array element and evaluate its (string) value as arithmetic,
// recursively -- like bash, where `a=b+1; b=3; echo $((a))' yields 4.
long long ref_get(const Node *n, Ctx &ctx) {
  std::string v;
  if (n->has_sub) v = ctx.sh.array_get(n->name, ctx.sh.zsh_subscript(n->name, n->sub));
  else { v = ctx.sh.get(n->name); if (v.empty()) { std::string dv; if (ctx.sh.dynamic_var(n->name, dv)) v = dv; } }
  if (v.empty()) return 0;
  if (ctx.depth > 100) return 0;
  bool o = true;
  long long r = eval_string(ctx.sh, v, ctx.depth + 1, &o);
  return o ? r : 0;
}
void ref_set(const Node *n, long long val, Ctx &ctx) {
  if (n->has_sub)
    ctx.sh.array_set(n->name, ctx.sh.zsh_subscript(n->name, n->sub), std::to_string(val));
  else if (!ctx.sh.set(n->name, std::to_string(val)))
    ctx.ok = false;  // assignment to a readonly variable: the expansion fails
}

long long eval_node(const Node *n, Ctx &ctx) {
  if (!n) return 0;
  // Operands are evaluated strictly left-to-right (bash's order), which matters
  // for side effects such as a++ + a++.
  auto A = [&]() { return eval_node(n->a.get(), ctx); };
  switch (n->k) {
    case K::Num: return n->num;
    case K::Var: return ref_get(n, ctx);
    case K::Neg: return -A();
    case K::LNot: return A() ? 0 : 1;
    case K::BNot: return ~A();
    case K::PreInc: { long long v = ref_get(n, ctx) + 1; ref_set(n, v, ctx); return v; }
    case K::PreDec: { long long v = ref_get(n, ctx) - 1; ref_set(n, v, ctx); return v; }
    case K::PostInc: { long long v = ref_get(n, ctx); ref_set(n, v + 1, ctx); return v; }
    case K::PostDec: { long long v = ref_get(n, ctx); ref_set(n, v - 1, ctx); return v; }
    case K::Mul: { long long l = A(); return l * eval_node(n->b.get(), ctx); }
    case K::Div: { long long l = A(), r = eval_node(n->b.get(), ctx); if (r == 0) { ctx.ok = false; return 0; } if (l == LLONG_MIN && r == -1) return LLONG_MIN; return l / r; }
    case K::Mod: { long long l = A(), r = eval_node(n->b.get(), ctx); if (r == 0) { ctx.ok = false; return 0; } if (l == LLONG_MIN && r == -1) return 0; return l % r; }
    // Exponentiation by squaring (like bash's ipow), so the cost is O(log e)
    // rather than O(e); a huge exponent no longer spins.  bash rejects a
    // negative exponent ("exponent less than 0") and defines e==0 as 1.
    case K::Pow: {
      long long base = A(), e = eval_node(n->b.get(), ctx), r = 1;
      if (e < 0) { ctx.ok = false; return 0; }
      while (e) { if (e & 1) r *= base; e >>= 1; if (e) base *= base; }
      return r;
    }
    case K::Add: { long long l = A(); return l + eval_node(n->b.get(), ctx); }
    case K::Sub: { long long l = A(); return l - eval_node(n->b.get(), ctx); }
    // Mask the shift count to [0,63] so the result is defined for out-of-range
    // or negative counts; this reproduces bash (e.g. 1<<64 == 1, 1<<-1 == 1<<63).
    case K::Shl: { long long l = A(); return l << (eval_node(n->b.get(), ctx) & 63); }
    case K::Shr: { long long l = A(); return l >> (eval_node(n->b.get(), ctx) & 63); }
    case K::Lt: { long long l = A(); return l < eval_node(n->b.get(), ctx) ? 1 : 0; }
    case K::Le: { long long l = A(); return l <= eval_node(n->b.get(), ctx) ? 1 : 0; }
    case K::Gt: { long long l = A(); return l > eval_node(n->b.get(), ctx) ? 1 : 0; }
    case K::Ge: { long long l = A(); return l >= eval_node(n->b.get(), ctx) ? 1 : 0; }
    case K::Eq: { long long l = A(); return l == eval_node(n->b.get(), ctx) ? 1 : 0; }
    case K::Ne: { long long l = A(); return l != eval_node(n->b.get(), ctx) ? 1 : 0; }
    case K::BAnd: { long long l = A(); return l & eval_node(n->b.get(), ctx); }
    case K::BXor: { long long l = A(); return l ^ eval_node(n->b.get(), ctx); }
    case K::BOr: { long long l = A(); return l | eval_node(n->b.get(), ctx); }
    case K::LAnd: { if (!A()) return 0; return eval_node(n->b.get(), ctx) ? 1 : 0; }  // short-circuit
    case K::LOr: { if (A()) return 1; return eval_node(n->b.get(), ctx) ? 1 : 0; }
    case K::Ternary: return A() ? eval_node(n->b.get(), ctx) : eval_node(n->c.get(), ctx);
    case K::Comma: A(); return eval_node(n->b.get(), ctx);
    case K::Assign: {
      long long rhs = A();
      long long res = rhs;
      const std::string o = n->aop ? n->aop : "=";
      if (o != "=") {
        long long cur = ref_get(n, ctx);
        if (o == "+=") res = cur + rhs;
        else if (o == "-=") res = cur - rhs;
        else if (o == "*=") res = cur * rhs;
        else if (o == "/=") { if (rhs == 0) { ctx.ok = false; res = 0; } else if (cur == LLONG_MIN && rhs == -1) res = LLONG_MIN; else res = cur / rhs; }
        else if (o == "%=") { if (rhs == 0) { ctx.ok = false; res = 0; } else if (cur == LLONG_MIN && rhs == -1) res = 0; else res = cur % rhs; }
        else if (o == "<<=") res = cur << (rhs & 63);
        else if (o == ">>=") res = cur >> (rhs & 63);
        else if (o == "&=") res = cur & rhs;
        else if (o == "^=") res = cur ^ rhs;
        else if (o == "|=") res = cur | rhs;
      }
      ref_set(n, res, ctx);
      return res;
    }
  }
  return 0;
}

}  // namespace

long long eval_arith(Shell &sh, const std::string &expr, bool *ok) {
  return eval_string(sh, expr, 0, ok);
}

}  // namespace gnash::core
