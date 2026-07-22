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
  size_t src = 0;             // start offset in the source expression (for errors)
};

// bash's arithmetic error messages, keyed for reuse by the parser/evaluator.
namespace arith_err {
constexpr const char *kOperand = "arithmetic syntax error: operand expected";
constexpr const char *kExpr = "arithmetic syntax error in expression";
constexpr const char *kDiv0 = "division by 0";
constexpr const char *kExponent = "exponent less than 0";
constexpr const char *kNonVar = "attempted assignment to non-variable";
constexpr const char *kMissingParen = "missing `)'";
constexpr const char *kExprExpected = "expression expected";
constexpr const char *kColonExpected = "`:' expected for conditional expression";
constexpr const char *kBadBase = "invalid arithmetic base";
constexpr const char *kBadConst = "invalid integer constant";
constexpr const char *kBadNumber = "invalid number";
constexpr const char *kTooGreat = "value too great for base";
constexpr const char *kLvalue = "assignment requires lvalue";
}  // namespace arith_err

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
  size_t last_tok = 0;                      // start of the most recently read token
  size_t err_pos = std::string::npos;       // start of the first error's token
  std::string err_msg;                      // first error's message
  explicit Parser(const std::string &str) : s(str) {}

  // Record the first error only (bash reports the earliest); P is the offset of
  // the error token, whose text is the remainder s.substr(P).
  void note(const std::string &m, size_t p) {
    if (err_pos == std::string::npos) { err_pos = p; err_msg = m; }
    ok = false;
  }

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
    if (s.compare(pos, n, op) == 0) { last_tok = pos; pos += n; return true; }
    return false;
  }
  // Record that a single-character operator at the current position is consumed.
  void mark_op() { last_tok = pos; }

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
    skip();
    size_t start = pos;
    n.name = read_name();
    if (n.name.empty()) return false;
    last_tok = start;
    n.src = start;
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
    while (peek() == ',') { mark_op(); pos++; v = binary(K::Comma, std::move(v), assignment()); }
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
          mark_op();
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
      mark_op(); pos++;
      auto n = mk(K::Ternary);
      n->a = std::move(c);
      skip();
      if (peek() == ':' || peek() == '\0')  // empty true branch
        note(arith_err::kExprExpected, pos);
      n->b = assignment();
      skip();
      if (peek() == ':') { mark_op(); pos++; }
      else note(arith_err::kColonExpected, last_tok);
      skip();
      if (peek() == '\0')  // empty false branch: report the ':'
        note(arith_err::kExprExpected, last_tok);
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
      if (peek() == '|' && s.compare(pos, 2, "||") != 0) { mark_op(); pos++; v = binary(K::BOr, std::move(v), bit_xor()); }
      else break;
    }
    return v;
  }
  NodeP bit_xor() {
    NodeP v = bit_and();
    while (peek() == '^') { mark_op(); pos++; v = binary(K::BXor, std::move(v), bit_and()); }
    return v;
  }
  NodeP bit_and() {
    NodeP v = equality();
    for (;;) { skip();
      if (peek() == '&' && s.compare(pos, 2, "&&") != 0) { mark_op(); pos++; v = binary(K::BAnd, std::move(v), equality()); }
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
      // A `+'/`-' after an operand is a binary operator even when another sign
      // follows (`4+++a' is `4 + ++a'); the trailing sign is handled as a unary
      // operator by the right operand.
      if (c == '+') { mark_op(); pos++; v = binary(K::Add, std::move(v), multiplicative()); }
      else if (c == '-') { mark_op(); pos++; v = binary(K::Sub, std::move(v), multiplicative()); }
      else break;
    }
    return v;
  }
  NodeP multiplicative() {
    NodeP v = power();
    for (;;) { skip(); char c = peek();
      if (c == '*' && s.compare(pos, 2, "**") != 0) { mark_op(); pos++; v = binary(K::Mul, std::move(v), power()); }
      else if (c == '/') { mark_op(); pos++; v = binary(K::Div, std::move(v), power()); }
      else if (c == '%') { mark_op(); pos++; v = binary(K::Mod, std::move(v), power()); }
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
    if (c == '+') { mark_op(); pos++; return unary(); }
    if (c == '-') { mark_op(); pos++; auto n = mk(K::Neg); n->a = unary(); n->src = n->a->src; return n; }
    if (c == '!') { mark_op(); pos++; auto n = mk(K::LNot); n->a = unary(); n->src = n->a->src; return n; }
    if (c == '~') { mark_op(); pos++; auto n = mk(K::BNot); n->a = unary(); n->src = n->a->src; return n; }
    return postfix();
  }
  NodeP preincr(K k) {
    size_t save = pos;
    auto n = mk(k);
    if (read_ref(*n)) return n;
    // `++'/`--' before a non-lvalue (e.g. ++7): bash evaluates the operand and
    // applies no increment, without error.  A wholly dangling `++'/`--' (no
    // operand at all) is an "operand expected" error whose token bash reports
    // as the second sign, since it rereads the pair as two unary operators.
    last_tok = save - 1;
    pos = save;
    return unary();
  }
  NodeP postfix() {
    size_t save = pos;
    auto ref = mk(K::Var);
    if (read_ref(*ref)) {
      skip();
      if (s.compare(pos, 2, "++") == 0) { mark_op(); pos += 2; ref->k = K::PostInc; return ref; }
      if (s.compare(pos, 2, "--") == 0) { mark_op(); pos += 2; ref->k = K::PostDec; return ref; }
      pos = save;  // not a postfix; re-read as a primary
    } else {
      pos = save;
    }
    return primary();
  }
  NodeP primary() {
    skip();
    if (peek() == '(') {
      mark_op(); pos++;
      NodeP v = comma();
      skip();
      if (peek() == ')') { mark_op(); pos++; }
      else note(arith_err::kMissingParen, last_tok);  // token = last token before EOF
      return v;
    }
    if (pos < s.size() && std::isdigit(static_cast<unsigned char>(s[pos]))) {
      size_t numstart = pos;
      last_tok = pos;
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
        if (base >= 2 && base <= 64 && any &&
            !(q < s.size() && (std::isalnum(static_cast<unsigned char>(s[q])) || s[q] == '#' ||
                               s[q] == '@' || s[q] == '_'))) {
          pos = q;
          auto n = mk(K::Num); n->num = v; n->src = numstart; return n;
        }
        // Malformed base#constant: classify the error the way bash does.
        const char *m;
        if (base > 64) m = arith_err::kBadBase;
        else if (base < 2) m = arith_err::kBadNumber;
        else if (!any)
          m = (q < s.size() && std::isalnum(static_cast<unsigned char>(s[q])))
                  ? arith_err::kTooGreat
                  : arith_err::kBadConst;
        else m = arith_err::kBadNumber;
        note(m, numstart);
        // Consume the whole token so parsing terminates.
        while (q < s.size() && (std::isalnum(static_cast<unsigned char>(s[q])) || s[q] == '#' ||
                                s[q] == '@' || s[q] == '_'))
          q++;
        pos = q;
        auto n = mk(K::Num); n->src = numstart; return n;
      }
      char *end = nullptr;
      long long v = std::strtoll(s.c_str() + pos, &end, 0);  // 0x/0 prefixes honored
      pos = static_cast<size_t>(end - s.c_str());
      auto n = mk(K::Num); n->num = v; n->src = numstart; return n;
    }
    size_t start = pos;
    auto ref = mk(K::Var);
    if (read_ref(*ref)) { ref->src = start; last_tok = start; return ref; }
    // Nothing parseable here: an operand was expected.  At end of input the
    // offending token is the dangling operator (last_tok); otherwise it is the
    // invalid token at the current position.
    note(arith_err::kOperand, pos >= s.size() ? last_tok : pos);
    return mk(K::Num);  // placeholder 0
  }
};

// A parsed expression plus whether it parsed cleanly (a malformed expression
// evaluates to a value but reports failure, matching the old behavior).  The
// AST is held by shared_ptr and returned BY VALUE, so a caller keeps it alive
// even if a nested evaluation clears the cache mid-walk.
struct Parsed {
  std::shared_ptr<Node> root;
  bool ok;
  size_t err_pos = std::string::npos;
  std::string err_msg;
};

Parsed parse_cached(const std::string &expr) {
  static std::map<std::string, Parsed> cache;
  auto it = cache.find(expr);
  if (it != cache.end()) return it->second;  // copies the shared_ptr
  if (cache.size() > 4096) cache.clear();     // bound memory (callers hold their own ref)
  Parser p(expr);
  std::shared_ptr<Node> root = p.comma();
  p.skip();
  // Tokens left after a complete expression: an assignment operator applied to a
  // non-lvalue, a bare ++/-- needing an lvalue, or a plain syntax error.
  if (p.err_pos == std::string::npos && p.pos < expr.size()) {
    size_t q = p.pos;
    auto two = expr.compare(q, 2, "==") == 0;  // not an assignment
    if (!two && expr[q] == '=')
      p.note(arith_err::kNonVar, q);
    else if (expr.compare(q, 2, "++") == 0 || expr.compare(q, 2, "--") == 0)
      p.note(expr.substr(q, 2) + ": " + arith_err::kLvalue, q);
    else if (q + 1 < expr.size() && expr[q + 1] == '=' &&
             std::strchr("+-*/%&^|", expr[q]) && expr.compare(q, 2, "&&") != 0 &&
             expr.compare(q, 2, "||") != 0)
      p.note(arith_err::kNonVar, q);  // +=, -=, ...: assign to non-lvalue
    else
      p.note(arith_err::kExpr, q);
  }
  Parsed parsed{std::move(root), p.ok && p.pos == expr.size(), p.err_pos, p.err_msg};
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

struct Ctx {
  Shell &sh;
  bool ok = true;
  int depth = 0;
  size_t err_pos = std::string::npos;  // offset of the error token (see Parser)
  std::string err_msg;
  void note(const std::string &m, size_t p) {
    if (err_pos == std::string::npos) { err_pos = p; err_msg = m; }
    ok = false;
  }
};

long long eval_node(const Node *n, Ctx &ctx);  // fwd

// True when the expression is empty or all whitespace (bash treats it as 0).
static bool blank_expr(const std::string &s) {
  for (char c : s) if (!std::isspace(static_cast<unsigned char>(c))) return false;
  return true;
}

long long eval_string(Shell &sh, const std::string &str, int depth, bool *ok) {
  long long iv;
  if (blank_expr(str)) { if (ok) *ok = true; return 0; }
  if (try_int(str, iv)) { if (ok) *ok = true; return iv; }
  Parsed p = parse_cached(str);
  Ctx ctx{sh, p.ok, depth, p.err_pos, p.err_msg};  // carry any parse error forward
  long long v = eval_node(p.root.get(), ctx);
  if (ok) *ok = ctx.ok;
  return v;
}

// Read a variable/array element and evaluate its (string) value as arithmetic,
// recursively -- like bash, where `a=b+1; b=3; echo $((a))' yields 4.
long long ref_get(const Node *n, Ctx &ctx) {
  std::string v;
  if (n->has_sub) {
    std::string sub = n->sub;
    if (!ctx.sh.array_expand_once_ok(n->name, sub)) { ctx.ok = false; return 0; }
    v = ctx.sh.array_get(n->name, ctx.sh.zsh_subscript(n->name, sub));
  }
  else { v = ctx.sh.get(n->name); if (v.empty()) { std::string dv; if (ctx.sh.dynamic_var(n->name, dv)) v = dv; } }
  if (v.empty()) return 0;
  if (ctx.depth > 100) return 0;
  bool o = true;
  long long r = eval_string(ctx.sh, v, ctx.depth + 1, &o);
  return o ? r : 0;
}
void ref_set(const Node *n, long long val, Ctx &ctx) {
  if (!ctx.ok) return;  // a read/subscript error already aborted the expression
  if (n->has_sub) {
    std::string sub = n->sub;
    if (!ctx.sh.array_expand_once_ok(n->name, sub)) { ctx.ok = false; return; }
    ctx.sh.array_set(n->name, ctx.sh.zsh_subscript(n->name, sub), std::to_string(val));
  } else if (!ctx.sh.set(n->name, std::to_string(val)))
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
    case K::Div: { long long l = A(), r = eval_node(n->b.get(), ctx); if (r == 0) { ctx.note(arith_err::kDiv0, n->b->src); return 0; } if (l == LLONG_MIN && r == -1) return LLONG_MIN; return l / r; }
    case K::Mod: { long long l = A(), r = eval_node(n->b.get(), ctx); if (r == 0) { ctx.note(arith_err::kDiv0, n->b->src); return 0; } if (l == LLONG_MIN && r == -1) return 0; return l % r; }
    // Exponentiation by squaring (like bash's ipow), so the cost is O(log e)
    // rather than O(e); a huge exponent no longer spins.  bash rejects a
    // negative exponent ("exponent less than 0") and defines e==0 as 1.
    case K::Pow: {
      long long base = A(), e = eval_node(n->b.get(), ctx), r = 1;
      if (e < 0) { ctx.note(arith_err::kExponent, n->b->src); return 0; }
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
        else if (o == "/=") { if (rhs == 0) { ctx.note(arith_err::kDiv0, n->a->src); res = 0; } else if (cur == LLONG_MIN && rhs == -1) res = LLONG_MIN; else res = cur / rhs; }
        else if (o == "%=") { if (rhs == 0) { ctx.note(arith_err::kDiv0, n->a->src); res = 0; } else if (cur == LLONG_MIN && rhs == -1) res = 0; else res = cur % rhs; }
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

// Like eval_arith, but on a syntax/evaluation error prints bash's diagnostic:
//   [SHELL: line N: ][CMD_NAME: ]EXPR: MESSAGE (error token is "TOKEN")
// CMD_NAME is "" for $((...)), "((" for the (( )) command, "let" for `let'.
long long eval_arith_msg(Shell &sh, const std::string &expr, const char *cmd_name,
                         bool *ok) {
  long long iv;
  if (blank_expr(expr)) { if (ok) *ok = true; return 0; }
  if (try_int(expr, iv)) { if (ok) *ok = true; return iv; }
  Parsed p = parse_cached(expr);
  Ctx ctx{sh, p.ok, 0, p.err_pos, p.err_msg};
  long long v = eval_node(p.root.get(), ctx);
  if (ok) *ok = ctx.ok;
  if (!ctx.ok && ctx.err_pos != std::string::npos && ctx.err_pos <= expr.size()) {
    size_t lead = 0;
    while (lead < expr.size() && std::isspace(static_cast<unsigned char>(expr[lead]))) lead++;
    std::string display = expr.substr(lead);
    std::string token = expr.substr(ctx.err_pos);
    // A malformed numeric constant's error token is the number itself, without
    // the trailing whitespace that padded the expression (`$(( 3425#56 ))' ->
    // `3425#56', not `3425#56 ').  Operator/operand-expected errors keep their
    // trailing space, so only trim for the number-constant diagnostics.
    if (ctx.err_msg == arith_err::kBadBase || ctx.err_msg == arith_err::kBadConst ||
        ctx.err_msg == arith_err::kBadNumber || ctx.err_msg == arith_err::kTooGreat) {
      auto rtrim = [](std::string &s) {
        while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
      };
      rtrim(display);
      rtrim(token);
    }
    std::string prefix = (cmd_name && cmd_name[0]) ? std::string(cmd_name) + ": " : "";
    std::fprintf(stderr, "%s%s%s: %s (error token is \"%s\")\n", sh.err_prefix().c_str(),
                 prefix.c_str(), display.c_str(), ctx.err_msg.c_str(), token.c_str());
  }
  return v;
}

}  // namespace gnash::core
