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

#include "gnash/core/expand.hpp"
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
constexpr const char *kRecursion = "expression recursion level exceeded";
constexpr const char *kBadOp = "arithmetic syntax error: invalid arithmetic operator";
}  // namespace arith_err

// The characters bash's is_arithop() accepts as operator tokens -- the
// single-character operators, plus `?', `:' and `,' (which its own comment
// calls "questionable").  Multi-character operators all begin with one of
// these, so testing the first character is enough.
bool arith_op_char(char c) {
  return c != '\0' && std::strchr("=><+-*/%!()&|^~?:,", c) != nullptr;
}

// What bash's readtok() will consume as an operand: a digit (NUM) or a
// variable starter (STR).  Anything else falls through to its operator
// classification.
bool arith_operand_start(char c) {
  unsigned char u = static_cast<unsigned char>(c);
  return std::isalnum(u) || c == '_';
}

NodeP mk(K k) { auto n = std::make_unique<Node>(); n->k = k; return n; }

// Parse an integer literal with bash's WRAPPING overflow semantics: expr.c's
// strlong accumulates `value*base + digit' with no overflow check, so
// 9223372036854775808 wraps to INTMAX_MIN.  strtoll would saturate at
// INTMAX_MAX, making `$(( -9223372036854775808 ))' come out one off.
// Honors an optional sign and the 0x/0X and leading-0 prefixes.
long long wrap_strtoll(const char *s, char **end) {
  const char *p = s;
  bool neg = false;
  if (*p == '+' || *p == '-') { neg = *p == '-'; p++; }
  unsigned base = 10;
  const char *fallback = p;  // where to leave *end if no digits follow a prefix
  if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) { base = 16; fallback = p + 1; p += 2; }
  else if (p[0] == '0') { base = 8; p += 1; fallback = p; }
  unsigned long long v = 0;
  const char *d0 = p;
  for (;; p++) {
    unsigned d;
    char c = *p;
    if (c >= '0' && c <= '9') d = static_cast<unsigned>(c - '0');
    else if (base == 16 && c >= 'a' && c <= 'f') d = static_cast<unsigned>(c - 'a') + 10;
    else if (base == 16 && c >= 'A' && c <= 'F') d = static_cast<unsigned>(c - 'A') + 10;
    else break;
    if (d >= base) break;
    v = v * base + d;
  }
  if (end) *end = const_cast<char *>(p == d0 ? fallback : p);
  long long r = static_cast<long long>(v);
  return neg ? -r : r;
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

  // Bound recursion so a deeply nested expression -- thousands of nested
  // parentheses or unary operators in $(( ... )) -- fails cleanly instead of
  // overflowing the call stack and crashing the shell.
  //
  // The bound is NOT bash's MAX_EXPR_RECURSION_LEVEL (1024): that counts nested
  // EVALUATION CONTEXTS -- a variable whose value is another expression -- not
  // descent through the precedence chain, which is why bash evaluates 20000
  // nested unary minuses in a single expression happily.  (That limit is
  // modelled separately, and is what `x=x; echo $((x))' reports.)
  //
  // So size this one against the stack instead.  Measured on this parser, the
  // descent survives 50000 frames and dies before 60000; 25000 keeps a 2x
  // margin under that while still accepting far deeper expressions than bash
  // itself manages -- it takes 50000 unary minuses to reach, and bash crashes
  // well before that.
  // Levels are CHARGED by what they cost the stack, because the shapes differ
  // by nearly an order of magnitude: one more unary operator is a single extra
  // frame, while one more parenthesis re-descends the whole precedence chain.
  // Measured here, the descent survives ~50000 unary levels but only ~4500
  // parenthesised ones, so a flat count safe for the first would crash on the
  // second.  kParenCost keeps both inside the same budget.
  int rec_depth = 0;
  static constexpr int kMaxDepth = 25000;
  static constexpr int kParenCost = 7;
  struct DepthGuard {
    Parser &p;
    int cost;
    bool allowed;
    explicit DepthGuard(Parser &pp, int c = 1) : p(pp), cost(c) {
      p.rec_depth += cost;
      allowed = p.rec_depth <= kMaxDepth;
      // Say why.  Without a note the failure surfaces as a syntax error about
      // whatever token the abandoned parse left behind, which describes the
      // symptom and not the cause.
      if (!allowed) p.note(arith_err::kRecursion, p.pos);
    }
    ~DepthGuard() { p.rec_depth -= cost; }
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
    // A \x04 display-escape before `[' still opens a subscript (expansion
    // output at expression level arrives escaped: `a\x04[\x04$k\x04]').
    if (pos + 1 < s.size() && s[pos] == '\x04' && s[pos + 1] == '[') pos++;
    if (pos < s.size() && s[pos] == '[') {
      pos++;
      int bdepth = 1;
      // The subscript is captured RAW for one-shot expansion at evaluation;
      // quoted spans and $(...) substitutions are opaque to the bracket
      // balance, so `assoc['$k']' and `a[$(cmd)]' span correctly.
      while (pos < s.size() && bdepth > 0) {
        char c2 = s[pos];
        if (c2 == '\x04' && pos + 1 < s.size()) {
          // Display-escaped brackets stay structural; other escaped chars are
          // carried (the escape is stripped again before expansion).
          if (s[pos + 1] == '[') { bdepth++; n.sub += '['; pos += 2; continue; }
          if (s[pos + 1] == ']') {
            if (--bdepth == 0) { pos += 2; break; }
            n.sub += ']';
            pos += 2;
            continue;
          }
          n.sub += c2; n.sub += s[pos + 1]; pos += 2; continue;
        }
        if (c2 == '\\' && pos + 1 < s.size()) {
          n.sub += c2; n.sub += s[pos + 1]; pos += 2; continue;
        }
        if (c2 == '\'' && s.find('\'', pos + 1) != std::string::npos) {
          // A PAIRED quote is a span (`assoc['$k']'); a lone one is data
          // (`a[80's]').
          n.sub += c2;
          while (++pos < s.size() && s[pos] != '\'') n.sub += s[pos];
          if (pos < s.size()) { n.sub += '\''; pos++; }
          continue;
        }
        if (c2 == '"' && s.find('"', pos + 1) != std::string::npos) {
          n.sub += c2;
          while (++pos < s.size() && s[pos] != '"') {
            if (s[pos] == '\\' && pos + 1 < s.size()) n.sub += s[pos++];
            n.sub += s[pos];
          }
          if (pos < s.size()) { n.sub += '"'; pos++; }
          continue;
        }
        if (c2 == '$' && pos + 1 < s.size() && s[pos + 1] == '(') {
          n.sub += "$(";
          pos += 2;
          int pd = 1;
          while (pos < s.size() && pd > 0) {
            if (s[pos] == '(') pd++;
            else if (s[pos] == ')') pd--;
            n.sub += s[pos];
            pos++;
          }
          continue;
        }
        if (c2 == '[') bdepth++;
        else if (c2 == ']') { if (--bdepth == 0) { pos++; break; } }
        if (bdepth > 0) n.sub += c2;
        pos++;
      }
      // An unterminated subscript (`b[$(echo' after word splitting) is
      // bash's "bad array subscript", blaming the reference.
      if (bdepth > 0) note("bad array subscript", start);
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
      if (peek() == '|' && s.compare(pos, 2, "||") != 0 && !at_op_assign()) { mark_op(); pos++; v = binary(K::BOr, std::move(v), bit_xor()); }
      else break;
    }
    return v;
  }
  NodeP bit_xor() {
    NodeP v = bit_and();
    while (peek() == '^' && !at_op_assign()) { mark_op(); pos++; v = binary(K::BXor, std::move(v), bit_and()); }
    return v;
  }
  NodeP bit_and() {
    NodeP v = equality();
    for (;;) { skip();
      if (peek() == '&' && s.compare(pos, 2, "&&") != 0 && !at_op_assign()) { mark_op(); pos++; v = binary(K::BAnd, std::move(v), equality()); }
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
      skip();
      if (s.compare(pos, 3, "<<=") == 0 || s.compare(pos, 3, ">>=") == 0) break;  // OP= token
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
      // operator by the right operand.  `+='/`-=' are single assignment tokens
      // (never `+' then `='): left for the caller, so `1 ? 20 : x+=2' reports
      // bash's "attempted assignment to non-variable" at the `+='.
      if (c == '+' && !at_op_assign()) { mark_op(); pos++; v = binary(K::Add, std::move(v), multiplicative()); }
      else if (c == '-' && !at_op_assign()) { mark_op(); pos++; v = binary(K::Sub, std::move(v), multiplicative()); }
      else break;
    }
    return v;
  }
  // The single-character operator at pos is really the head of an OP=
  // assignment token.
  bool at_op_assign() { return pos + 1 < s.size() && s[pos + 1] == '='; }
  NodeP multiplicative() {
    NodeP v = power();
    for (;;) { skip(); char c = peek();
      if (c == '*' && s.compare(pos, 2, "**") != 0 && !at_op_assign()) { mark_op(); pos++; v = binary(K::Mul, std::move(v), power()); }
      else if (c == '/' && !at_op_assign()) { mark_op(); pos++; v = binary(K::Div, std::move(v), power()); }
      else if (c == '%' && !at_op_assign()) { mark_op(); pos++; v = binary(K::Mod, std::move(v), power()); }
      else break;
    }
    return v;
  }
  NodeP power() {
    NodeP base = unary();
    if (eat("**")) return binary(K::Pow, std::move(base), power());  // right-associative
    return base;
  }
  // A postfix `++'/`--' after an expression that is not a bare variable --
  // e.g. `--x++', where the `++' would apply to the value of `--x' -- is
  // bash's "++: assignment requires lvalue" (the lexer saw a variable last,
  // so the pair reads as a postfix operator, then fails the lvalue check).
  NodeP postlv_check(NodeP n) {
    skip();
    if ((n->k == K::PreInc || n->k == K::PreDec) && !n->name.empty() &&
        (s.compare(pos, 2, "++") == 0 || s.compare(pos, 2, "--") == 0))
      note(s.substr(pos, 2) + ": " + arith_err::kLvalue, pos);
    return n;
  }
  NodeP unary() {
    DepthGuard g(*this);
    if (!g.allowed) return mk(K::Num);  // too deep: bail with a placeholder
    skip();
    if (eat("++")) return postlv_check(preincr(K::PreInc));
    if (eat("--")) return postlv_check(preincr(K::PreDec));
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
      DepthGuard g(*this, kParenCost);
      if (!g.allowed) return mk(K::Num);
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
      // Integer literal: a port of bash's strlong (expr.c).  readtok reads the
      // whole `[alnum#@_]' run as ONE token and strlong scans it left to right
      // under a single running base.  A leading `0' selects octal (`0x'/`0X'
      // hex) and marks the base as found; a `#' takes the value read so far
      // as the base (2..64, else `invalid arithmetic base') -- unless a base
      // was already found (`0#1', `010#1', `2#1#1': `invalid number').  Each
      // digit character maps through the full 0-9 a-z A-Z @ _ alphabet and is
      // checked against the CURRENT base, so `08#1' fails on the `8' (`value
      // too great for base') before its `#' is ever seen, as do `0xg', `08'
      // and `2#1x'.  A `#' must be followed by a digit character (`2#',
      // `2##1': `invalid integer constant').  Values accumulate with wrapping
      // and no overflow check, so 9223372036854775808 wraps to INTMAX_MIN.
      // An empty run after `0x', or a lone `0', is the value 0.
      size_t q = pos;
      while (q < s.size() && (std::isalnum(static_cast<unsigned char>(s[q])) || s[q] == '#' ||
                              s[q] == '@' || s[q] == '_'))
        q++;
      const char *err = nullptr;
      unsigned long long base = 10, val = 0;
      bool foundbase = false;
      size_t k = pos;
      if (s[k] == '0') {
        k++;
        if (k < q && (s[k] == 'x' || s[k] == 'X')) { base = 16; k++; }
        else base = 8;
        foundbase = true;
      }
      for (; k < q && !err; k++) {
        unsigned char c = static_cast<unsigned char>(s[k]);
        if (c == '#') {
          if (foundbase) { err = arith_err::kBadNumber; break; }
          long long b = static_cast<long long>(val);  // bash compares the signed value
          if (b < 2 || b > 64) { err = arith_err::kBadBase; break; }
          base = val;
          val = 0;
          foundbase = true;
          if (k + 1 >= q || s[k + 1] == '#') err = arith_err::kBadConst;
          continue;
        }
        unsigned long long d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'z') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'Z') d = c - 'A' + (base <= 36 ? 10 : 36);
        else if (c == '@') d = 62;
        else d = 63;  // '_'
        if (d >= base) { err = arith_err::kTooGreat; break; }
        val = val * base + d;
      }
      pos = q;
      auto n = mk(K::Num);
      n->src = numstart;
      if (err) note(err, numstart);
      else n->num = static_cast<long long>(val);
      return n;
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
    // bash's readtok() splits the leftover into two diagnostics.  A character
    // that could begin an OPERAND (a digit, or a variable starter) is read as a
    // token, and it is the grammar that then rejects it -- "syntax error in
    // expression".  A character that is neither an operand starter nor one of
    // the operator characters is rejected by the tokenizer itself, and since
    // what precedes it here is a complete expression (so bash's `curtok' is an
    // operand rather than an operator), that is "invalid arithmetic operator".
    else if (!arith_operand_start(expr[q]) && !arith_op_char(expr[q]))
      p.note(arith_err::kBadOp, q);
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
  out = wrap_strtoll(s.c_str() + start, &end);
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
  const char *cmd_name = nullptr;  // "(("/"let" -- prefixes a nameref-target error
  // A fully formatted diagnostic from a NESTED evaluation (a variable's value
  // failing to evaluate names the VALUE as the expression, not the outer
  // expression: `A="4 + "; $((A))' -> `4 + : ... operand expected').  When
  // set, eval_arith_msg prints it verbatim instead of formatting err_msg.
  std::string full_msg;
  // full_msg prints WITHOUT the command prefix (bash reports a subscript
  // evaluation failure from the array layer, with no `((: ').
  bool full_msg_bare = false;
  // How subscripts are treated: 0 = legacy pre-expanded, silent ([[ and
  // internal callers); 1 = arithmetic SOURCE text ((( )), $(( )), $[ ]) --
  // the raw subscript is word-expanded once by the evaluator; 2 = already
  // word-expanded arithmetic (`let') -- no re-expansion, but double-quote
  // CHARS in the text are removable arithmetic quoting.
  int expand_subs = 0;
  void note(const std::string &m, size_t p) {
    if (err_pos == std::string::npos) { err_pos = p; err_msg = m; }
    ok = false;
  }
};

// Format bash's arithmetic diagnostic body: `EXPR: MESSAGE (error token is
// "TOKEN")', with the number-constant messages trimming trailing whitespace.
static std::string format_arith_err(const std::string &expr, const std::string &msg,
                                    size_t err_pos) {
  size_t lead = 0;
  while (lead < expr.size() && std::isspace(static_cast<unsigned char>(expr[lead]))) lead++;
  std::string display = expr.substr(lead);
  std::string token = err_pos <= expr.size() ? expr.substr(err_pos) : std::string();
  if (msg == arith_err::kBadBase || msg == arith_err::kBadConst ||
      msg == arith_err::kBadNumber || msg == arith_err::kTooGreat) {
    // A malformed NUMBER names just its own token (bash's readtok reads the
    // `[alnum#@_]' run), not the rest of the expression: `0xg+1' -> `0xg',
    // `2#5+1' -> `2#5'.  bash also NUL-terminates the token while reading it,
    // so the displayed EXPRESSION is truncated at the token's END -- `08*2'
    // shows `08' but `3+08' shows the whole `3+08'.
    size_t e = 0;
    while (e < token.size() && (std::isalnum(static_cast<unsigned char>(token[e])) ||
                                token[e] == '#' || token[e] == '@' || token[e] == '_'))
      e++;
    token.resize(e);
    if (err_pos + e >= lead) display = expr.substr(lead, err_pos + e - lead);
  }
  return display + ": " + msg + " (error token is \"" + token + "\")";
}

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
  Ctx ctx{sh, p.ok, depth, p.err_pos, p.err_msg, nullptr, {}, false, 0};  // carry any parse error forward
  long long v = eval_node(p.root.get(), ctx);
  if (ok) *ok = ctx.ok;
  return v;
}

// Resolve an lvalue's subscript ONCE, so a read-modify-write (`d[x++]++',
// `++d[RANDOM]', `d[i--]+=2') runs the subscript's side effects a single time,
// as bash does.  For an indexed-array target the subscript arithmetic is
// evaluated here (the numeric result re-evaluates idempotently in
// array_get/array_set, which still apply negative-index resolution); an
// associative key passes through untouched.
static bool resolve_sub(const Node *n, Ctx &ctx, std::string &out, bool writing = false) {
  out = n->sub;
  auto kind_of = [&]() {
    auto it = ctx.sh.vars.find(ctx.sh.deref(n->name));
    return it != ctx.sh.vars.end() && it->second.kind == VarKind::Assoc;
  };
  if (ctx.expand_subs == 0) {
    // Pre-expanded context ([[ operands, let, assignments): the text IS the
    // final subscript; nothing here may expand or execute it again.
    if (!ctx.sh.array_expand_once_ok(n->name, out)) { ctx.ok = false; return false; }
    out = ctx.sh.zsh_subscript(n->name, out);
    if (!kind_of()) {
      if (blank_expr(out)) { out = "0"; return true; }
      long long k;
      if (!try_int(out, k)) {
        Parsed p = parse_cached(out);
        Ctx sc{ctx.sh, p.ok, ctx.depth + 1, p.err_pos, p.err_msg, ctx.cmd_name, {}, false, 0};
        k = eval_node(p.root.get(), sc);
        if (!sc.ok) {
          ctx.ok = false;
          // `let' reports a bad subscript (bare, from the array layer);
          // other pre-expanded contexts stay silent as before.
          if (ctx.cmd_name && std::strcmp(ctx.cmd_name, "let") == 0) {
            ctx.full_msg = !sc.full_msg.empty()
                               ? sc.full_msg
                               : format_arith_err(out, sc.err_msg, sc.err_pos);
            ctx.full_msg_bare = true;
          }
          return false;
        }
      }
      out = std::to_string(k);
    }
    return true;
  }
  // Arithmetic context: the raw subscript is expanded exactly ONCE here
  // (parameter/command/arithmetic expansion with quote removal), as bash
  // does at evaluation: `assoc[$key]' keys on $key's VALUE, `assoc['$key']'
  // on the literal string $key -- and the result is never re-scanned, so a
  // `]' or `$(' in a value is inert data.
  // Strip the preprocessor's \x04 display escapes: the escaped text is the
  // real subscript.
  std::string raw;
  for (size_t k = 0; k < out.size(); k++) {
    if (out[k] == '\x04' && k + 1 < out.size()) continue;
    raw += out[k];
  }
  if (ctx.expand_subs == 1 && raw.find_first_of("$`'\"\\") != std::string::npos) {
    Expander ex(ctx.sh);
    out = ex.expand_no_split(raw, false, /*do_procsub=*/false);
  } else {
    out = raw;
  }
  out = ctx.sh.zsh_subscript(n->name, out);
  if (kind_of()) {
    // An associative key: the once-expanded text is the key, verbatim for the
    // arithmetic-source mode (`(( a[\" \"]=11 ))' keys on the literal `" "');
    // let's pre-expanded text drops its removable double-quote characters
    // (`let 'a[" "]=11'` keys on the space) -- EXCEPT under assoc_expand_once,
    // whose literal-subscript rule keeps them (`let "a[\\" \\"]=11"' keys on
    // `" "' with the quotes, array25.sub).
    bool eo = false;
    {
      auto eoit = ctx.sh.shopt_opts.find("assoc_expand_once");
      eo = eoit != ctx.sh.shopt_opts.end() && eoit->second;
    }
    if (ctx.expand_subs == 2 && !eo) {
      std::string k2;
      for (char c : out)
        if (c != '"') k2 += c;
      out = k2;
    }
    return true;
  }
  // An indexed (or undeclared) target: an EMPTY word-expanded subscript is
  // bash's `a[]': not a valid identifier; `@'/`*' is a bad array subscript;
  // otherwise double-quote CHARS are removable arithmetic quoting and the
  // result is arith-evaluated, a malformed one aborting with bash's
  // diagnostic naming the SUBSCRIPT.
  if (out.empty()) {
    // An EMPTY subscript is reported but is NOT fatal to the expression: bash
    // prints the diagnostic, then carries on -- a read yields 0, a write is
    // skipped while the assignment still evaluates to its right-hand side, and
    // the enclosing command list keeps running.  (A real syntax error, by
    // contrast, still unwinds; see the arith_abort path in expand.cpp.)  So
    // ctx.ok is deliberately left alone here: returning false already gives
    // the caller both behaviours.
    if (writing) {
      // `(( a[""]=24 ))' -- an assignment target: `a[]': not a valid
      // identifier (with the command prefix).
      // Unlike the read below, this one carries the command prefix (`((: ').
      std::fprintf(stderr, "%s%s%s`%s[]': not a valid identifier\n",
                   ctx.sh.err_prefix().c_str(),
                   (ctx.cmd_name && *ctx.cmd_name) ? ctx.cmd_name : "",
                   (ctx.cmd_name && *ctx.cmd_name) ? ": " : "", n->name.c_str());
    } else {
      // `(( y[$none] ))' -- a read: y[]: bad array subscript (bare).  bash
      // reports it TWICE: once while resolving the reference and again when
      // the arithmetic command reports the failed expression (array27.sub).
      std::string m = n->name + "[]: bad array subscript";
      std::fprintf(stderr, "%s%s\n", ctx.sh.err_prefix().c_str(), m.c_str());
      if (ctx.expand_subs == 1)
        std::fprintf(stderr, "%s%s\n", ctx.sh.err_prefix().c_str(), m.c_str());
    }
    return false;
  }
  if (out == "@" || out == "*") {
    ctx.ok = false;
    ctx.full_msg = n->name + "[" + out + "]: bad array subscript";
    ctx.full_msg_bare = true;
    return false;
  }
  // Double-quote CHARACTERS in an indexed subscript are removable arithmetic
  // quoting -- EXCEPT under assoc_expand_once, whose literal-subscript rule
  // keeps them for an indexed array too, so `let "a[\" \"]"=18' hands `" "' to
  // the evaluator and is a syntax error rather than a write to element 0.
  bool eo_idx = false;
  {
    auto eoit = ctx.sh.shopt_opts.find("assoc_expand_once");
    eo_idx = eoit != ctx.sh.shopt_opts.end() && eoit->second;
  }
  std::string ev;
  if (ctx.expand_subs == 2 && eo_idx) {
    ev = out;
  } else {
    for (char c : out)
      if (c != '"') ev += c;
  }
  if (blank_expr(ev)) { out = "0"; return true; }
  long long k;
  if (!try_int(ev, k)) {
    Parsed p = parse_cached(ev);
    Ctx sc{ctx.sh, p.ok, ctx.depth + 1, p.err_pos, p.err_msg, ctx.cmd_name, {}, false, ctx.expand_subs};
    k = eval_node(p.root.get(), sc);
    if (!sc.ok) {
      ctx.ok = false;
      ctx.full_msg = !sc.full_msg.empty()
                         ? sc.full_msg
                         : format_arith_err(ev, sc.err_msg, sc.err_pos);
      ctx.full_msg_bare = true;
      return false;
    }
  }
  out = std::to_string(k);
  return true;
}

// Read a variable/array element and evaluate its (string) value as arithmetic,
// recursively -- like bash, where `a=b+1; b=3; echo $((a))' yields 4.  RSUB,
// when given, is the already-resolved subscript from resolve_sub.
long long ref_get(const Node *n, Ctx &ctx, const std::string *rsub = nullptr) {
  std::string v;
  bool have = true;
  if (n->has_sub) {
    if (rsub) {
      v = ctx.sh.array_get(n->name, *rsub);
    } else {
      std::string rs;
      if (!resolve_sub(n, ctx, rs)) return 0;
      v = ctx.sh.array_get(n->name, rs);
    }
    // For nounset, an element of a wholly unset variable counts as unbound
    // (bash blames the bare NAME: `a[0] > 4' -> `a: unbound variable').
    have = ctx.sh.vars.count(ctx.sh.deref(n->name)) != 0;
  } else if (!ctx.sh.get_if_set(n->name, v)) {
    std::string dv;
    if (ctx.sh.dynamic_var(n->name, dv)) v = dv;
    else have = false;
  }
  if (!have && ctx.sh.opt_nounset) {
    // set -u: an unset variable in arithmetic aborts like any other expansion
    // (non-interactive bash exits with 127); the message is already printed,
    // so fail without queueing a second diagnostic.
    std::fprintf(stderr, "%s%s: unbound variable\n", ctx.sh.err_prefix().c_str(),
                 n->name.c_str());
    ctx.sh.exiting = true;
    ctx.sh.exit_status = 127;
    ctx.ok = false;
    return 0;
  }
  if (blank_expr(v)) return 0;
  long long iv;
  if (try_int(v, iv)) return iv;
  // bash caps value-recursion (`a=b; b=a; $((a))') at MAX_EXPR_RECURSION_LEVEL
  // and blames the value that would evaluate at the tripping level.
  if (ctx.depth + 1 >= 1023) {
    ctx.full_msg = format_arith_err(v, arith_err::kRecursion, 0);
    ctx.ok = false;
    return 0;
  }
  // Evaluate the value with error PROPAGATION: a malformed value (`A="4 + ";
  // $(( A + 4 ))') aborts the whole expression, and the diagnostic names the
  // VALUE as the failing expression, exactly as bash reports it.
  Parsed p = parse_cached(v);
  Ctx sub{ctx.sh, p.ok, ctx.depth + 1, p.err_pos, p.err_msg, ctx.cmd_name, {}, false, ctx.expand_subs};
  long long r = eval_node(p.root.get(), sub);
  if (!sub.ok) {
    ctx.ok = false;
    if (!sub.full_msg.empty()) {
      ctx.full_msg = sub.full_msg;
      ctx.full_msg_bare = sub.full_msg_bare;
    } else if (sub.err_pos != std::string::npos) {
      ctx.full_msg = format_arith_err(v, sub.err_msg, sub.err_pos);
    }
    return 0;
  }
  return r;
}
void ref_set(const Node *n, long long val, Ctx &ctx, const std::string *rsub = nullptr) {
  if (!ctx.ok) return;  // a read/subscript error already aborted the expression
  if (n->has_sub) {
    if (rsub) {
      ctx.sh.array_set(n->name, *rsub, std::to_string(val));
      return;
    }
    std::string rs;
    if (!resolve_sub(n, ctx, rs, /*writing=*/true)) return;
    ctx.sh.array_set(n->name, rs, std::to_string(val));
    return;
  }
  // A bare array name in arithmetic reads and WRITES element 0 (`x=(1 2);
  // ((x=9))' yields x=([0]=9 [1]=2) in bash) -- Shell::set would ignore it.
  auto it = ctx.sh.vars.find(ctx.sh.deref(n->name));
  if (it != ctx.sh.vars.end() &&
      (it->second.kind == VarKind::Indexed || it->second.kind == VarKind::Assoc)) {
    ctx.sh.array_set(n->name, "0", std::to_string(val));
    return;
  }
  if (!ctx.sh.set(n->name, std::to_string(val), ctx.cmd_name))
    ctx.ok = false;  // readonly, or an invalid nameref target (`((: `0': ...')
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
    case K::PreInc: case K::PreDec: case K::PostInc: case K::PostDec: {
      // Read-modify-write: resolve any subscript once (its side effects must
      // not run again for the write back).
      std::string rs;
      const std::string *rp = nullptr;
      if (n->has_sub) { if (!resolve_sub(n, ctx, rs, /*writing=*/true)) return 0; rp = &rs; }
      long long cur = ref_get(n, ctx, rp);
      long long d = (n->k == K::PreInc || n->k == K::PostInc) ? 1 : -1;
      ref_set(n, cur + d, ctx, rp);
      return (n->k == K::PreInc || n->k == K::PreDec) ? cur + d : cur;
    }
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
      // For OP= the subscript is resolved once and shared by the read and the
      // write back (`d[x++]+=2' bumps x a single time, as bash).
      std::string rs;
      const std::string *rp = nullptr;
      if (o != "=" && n->has_sub) { if (!resolve_sub(n, ctx, rs, /*writing=*/true)) return 0; rp = &rs; }
      if (o != "=") {
        long long cur = ref_get(n, ctx, rp);
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
      ref_set(n, res, ctx, rp);
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
                         bool *ok, int expand_subs) {
  long long iv;
  if (blank_expr(expr)) { if (ok) *ok = true; return 0; }
  if (try_int(expr, iv)) { if (ok) *ok = true; return iv; }
  Parsed p = parse_cached(expr);
  bool parse_failed = !p.ok;
  Ctx ctx{sh, p.ok, 0, p.err_pos, p.err_msg, cmd_name, {}, false, expand_subs};
  long long v = eval_node(p.root.get(), ctx);
  if (ok) *ok = ctx.ok;
  // A PARSE error outranks an evaluation-side diagnostic (`let 'a[x],b[$(e'
  // reports the bad subscript found at parse, not the doomed evaluation).
  if (parse_failed) ctx.full_msg.clear();
  std::string prefix = (cmd_name && cmd_name[0]) ? std::string(cmd_name) + ": " : "";
  // \x04 display-escape markers in expansion output: the (( )) command's
  // diagnostics render them as backslashes (`'assoc[x\],b\[\$(...)]++'`);
  // $(( )) diagnostics drop them (`$( echo >&2 foo ) : ...`), as bash.
  auto rendere = [&](std::string t) {
    std::string r;
    for (char c : t) {
      if (c == '\x04') {
        if (cmd_name && cmd_name[0]) r += '\\';
        continue;
      }
      r += c;
    }
    return r;
  };
  if (!ctx.ok && !ctx.full_msg.empty()) {
    // A nested value-evaluation failure carries its own fully formatted
    // diagnostic naming the VALUE as the expression; a subscript failure
    // prints bare (no `((: ' prefix), as bash's array layer does.
    std::fprintf(stderr, "%s%s%s\n", sh.err_prefix().c_str(),
                 ctx.full_msg_bare ? "" : prefix.c_str(), rendere(ctx.full_msg).c_str());
    // A failure raised by the ARRAY layer while evaluating a subscript unwinds
    // the command list, even from `let', where a failure of the top-level
    // expression (`let "x+"') merely returns non-zero and execution continues.
    if (ctx.full_msg_bare) sh.arith_abort = true;
  } else if (!ctx.ok && ctx.err_pos != std::string::npos && ctx.err_pos <= expr.size()) {
    std::fprintf(stderr, "%s%s%s\n", sh.err_prefix().c_str(), prefix.c_str(),
                 rendere(format_arith_err(expr, ctx.err_msg, ctx.err_pos)).c_str());
  }
  return v;
}

}  // namespace gnash::core
