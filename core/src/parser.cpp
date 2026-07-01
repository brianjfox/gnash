// parser.cpp -- recursive-descent parser (see parser.hpp).

#include "gnash/core/parser.hpp"

#include <cctype>
#include <cstdlib>
#include <initializer_list>
#include <memory>

#include "gnash/core/lexer.hpp"

namespace gnash::core {

namespace {

bool is_name(const std::string &s) {
  if (s.empty()) return false;
  if (!(std::isalpha(static_cast<unsigned char>(s[0])) || s[0] == '_')) return false;
  for (char c : s)
    if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_')) return false;
  return true;
}

std::string trim(const std::string &s) {
  std::size_t a = 0, b = s.size();
  while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) a++;
  while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) b--;
  return s.substr(a, b - a);
}

std::string tok_to_text(const Token &t) {
  if (t.type == Tok::Word || t.type == Tok::IoNumber) return t.text;
  return tok_name(t.type);
}

bool is_cond_unary(const std::string &w) {
  return w.size() == 2 && w[0] == '-' && std::isalpha(static_cast<unsigned char>(w[1]));
}

bool is_cond_binop_word(const std::string &w) {
  static const char *ops[] = {"==", "=",   "!=",  "=~",  "-eq", "-ne", "-lt",
                              "-le", "-gt", "-ge", "-nt", "-ot", "-ef", nullptr};
  for (int i = 0; ops[i]; i++)
    if (w == ops[i]) return true;
  return false;
}

bool is_compound_kw(const std::string &w) {
  static const char *kw[] = {"{",  "if",     "while", "until", "for",
                             "select", "case", "[[", "function", nullptr};
  for (int i = 0; kw[i]; i++)
    if (w == kw[i]) return true;
  return false;
}

struct Parser {
  std::vector<Token> toks;
  std::size_t i = 0;
  bool err = false;
  bool incomplete = false;
  std::string errmsg;

  explicit Parser(std::vector<Token> t) : toks(std::move(t)) {}

  const Token &cur() const { return toks[i]; }
  const Token &peek(std::size_t k) const {
    std::size_t j = i + k;
    return j < toks.size() ? toks[j] : toks.back();
  }
  void advance() {
    if (i + 1 < toks.size()) i++;
  }
  bool is(Tok t) const { return cur().type == t; }

  bool reserved(const char *w) const {
    return cur().type == Tok::Word && !cur().quoted && cur().text == w;
  }
  bool reserved_in(std::initializer_list<const char *> ws) const {
    if (cur().type != Tok::Word || cur().quoted) return false;
    for (const char *w : ws)
      if (cur().text == w) return true;
    return false;
  }

  void fail(const std::string &m) {
    if (!err) {
      err = true;
      errmsg = m;
    }
  }
  void expect_reserved(const char *w) {
    if (reserved(w))
      advance();
    else {
      if (is(Tok::Eof)) incomplete = true;
      fail(std::string("expected `") + w + "'");
    }
  }
  void expect(Tok t, const char *name) {
    if (is(t))
      advance();
    else {
      if (is(Tok::Eof)) incomplete = true;
      fail(std::string("expected `") + name + "'");
    }
  }

  void newline_list() {
    while (is(Tok::Newline)) advance();
  }

  // -- redirections --------------------------------------------------------
  bool at_redirect() const {
    switch (cur().type) {
      case Tok::Less:
      case Tok::Great:
      case Tok::DGreat:
      case Tok::DLess:
      case Tok::DLessDash:
      case Tok::TLess:
      case Tok::LessAnd:
      case Tok::GreatAnd:
      case Tok::LessGreat:
      case Tok::Clobber:
      case Tok::AndGreat:
      case Tok::AndDGreat:
        return true;
      case Tok::IoNumber:
        return true;
      default:
        return false;
    }
  }

  bool parse_redirect(std::vector<Redirect> &redirs) {
    Redirect r;
    if (is(Tok::IoNumber)) {
      r.source_fd = std::atoi(cur().text.c_str());
      advance();
    }
    Tok op = cur().type;
    Token optok = cur();
    switch (op) {
      case Tok::Less: r.op = RedirOp::InputRedir; break;
      case Tok::Great: r.op = RedirOp::OutputRedir; break;
      case Tok::DGreat: r.op = RedirOp::AppendOutput; break;
      case Tok::Clobber: r.op = RedirOp::Clobber; break;
      case Tok::LessGreat: r.op = RedirOp::InputOutput; break;
      case Tok::LessAnd: r.op = RedirOp::DupInput; break;
      case Tok::GreatAnd: r.op = RedirOp::DupOutput; break;
      case Tok::TLess: r.op = RedirOp::HereString; break;
      case Tok::DLess: r.op = RedirOp::HereDoc; break;
      case Tok::DLessDash: r.op = RedirOp::HereDocStrip; break;
      case Tok::AndGreat: r.op = RedirOp::AndOutput; break;
      case Tok::AndDGreat: r.op = RedirOp::AndAppend; break;
      default:
        fail("bad redirection operator");
        return false;
    }
    advance();
    if (cur().type != Tok::Word) {
      fail("expected redirection target");
      return false;
    }
    r.target = Word{cur().text, cur().quoted ? W_QUOTED : 0};
    if (cur().has_heredoc) {
      r.heredoc_body = cur().heredoc_body;
      r.heredoc_quoted = cur().heredoc_quoted;
    }
    advance();
    (void)optok;
    redirs.push_back(std::move(r));
    return true;
  }

  void parse_redirect_list(std::vector<Redirect> &redirs) {
    while (!err && at_redirect()) parse_redirect(redirs);
  }

  // -- lists ---------------------------------------------------------------
  bool at_list_end(std::initializer_list<const char *> stops) const {
    if (is(Tok::Eof) || is(Tok::Rparen)) return true;
    if (is(Tok::SemiSemi) || is(Tok::SemiAnd) || is(Tok::SemiSemiAnd)) return true;
    return reserved_in(stops);
  }

  static CommandPtr connect(Connector c, CommandPtr a, CommandPtr b) {
    auto n = std::make_unique<Connection>();
    n->conn = c;
    n->first = std::move(a);
    n->second = std::move(b);
    return n;
  }

  CommandPtr parse_list(std::initializer_list<const char *> stops) {
    newline_list();
    if (at_list_end(stops)) return nullptr;
    CommandPtr left = parse_and_or(stops);
    while (!err) {
      Connector conn;
      if (is(Tok::Semi))
        conn = Connector::Semi;
      else if (is(Tok::Amp))
        conn = Connector::Amp;
      else if (is(Tok::Newline))
        conn = Connector::Newline;
      else
        break;
      advance();
      newline_list();
      if (at_list_end(stops)) {
        if (conn == Connector::Amp) left = connect(Connector::Amp, std::move(left), nullptr);
        break;
      }
      CommandPtr right = parse_and_or(stops);
      left = connect(conn, std::move(left), std::move(right));
    }
    return left;
  }

  CommandPtr parse_and_or(std::initializer_list<const char *> stops) {
    CommandPtr left = parse_pipeline(stops);
    while (!err && (is(Tok::AndAnd) || is(Tok::OrOr))) {
      Connector conn = is(Tok::AndAnd) ? Connector::And : Connector::Or;
      advance();
      newline_list();
      CommandPtr right = parse_pipeline(stops);
      left = connect(conn, std::move(left), std::move(right));
    }
    return left;
  }

  bool at_command_start() const {
    switch (cur().type) {
      case Tok::Eof:
      case Tok::Newline:
      case Tok::Semi:
      case Tok::Amp:
      case Tok::Pipe:
      case Tok::PipeAnd:
      case Tok::AndAnd:
      case Tok::OrOr:
      case Tok::Rparen:
      case Tok::SemiSemi:
      case Tok::SemiAnd:
      case Tok::SemiSemiAnd:
        return false;
      default:
        return !reserved_in({"then", "do", "done", "fi", "elif", "else", "esac", "}"});
    }
  }

  CommandPtr parse_pipeline(std::initializer_list<const char *> stops) {
    int cmdflags = 0;
    bool had_prefix = false;
    // Leading `!' and `time' may appear in either order (POSIX timed/negated
    // pipelines: `time ! cmd', `! time cmd', and a bare `!').
    for (;;) {
      if (reserved("!")) {
        cmdflags ^= CMD_INVERT_RETURN;
        had_prefix = true;
        advance();
        continue;
      }
      if (reserved("time")) {
        cmdflags |= CMD_TIME;
        had_prefix = true;
        advance();
        if (reserved("-p")) {
          cmdflags |= CMD_TIME_POSIX;
          advance();
        }
        continue;
      }
      break;
    }

    // A bare `!'/`time' with no following command negates/times the null command.
    if (had_prefix && !at_command_start()) {
      auto e = std::make_unique<SimpleCommand>();
      e->flags |= cmdflags;
      return e;
    }

    CommandPtr left = parse_command(stops);
    while (!err && (is(Tok::Pipe) || is(Tok::PipeAnd))) {
      bool piperr = is(Tok::PipeAnd);
      advance();
      newline_list();
      CommandPtr right = parse_command(stops);
      if (piperr && left) {
        // |& duplicates stderr onto stdout for the left-hand command.
        Redirect r;
        r.source_fd = 2;
        r.op = RedirOp::DupOutput;
        r.target = Word{"1", 0};
        left->redirects.push_back(std::move(r));
      }
      left = connect(Connector::Pipe, std::move(left), std::move(right));
    }
    if (left) left->flags |= cmdflags;
    return left;
  }

  // -- commands ------------------------------------------------------------
  CommandPtr parse_command(std::initializer_list<const char *> stops) {
    if (is(Tok::Lparen)) {
      if (cur().glued && peek(1).type == Tok::Lparen) return parse_arith_command();
      return parse_subshell();
    }
    if (reserved("{")) return parse_group();
    if (reserved("[[")) return parse_cond();
    if (reserved("if")) return parse_if();
    if (reserved("while") || reserved("until")) return parse_loop();
    if (reserved("for") || reserved("select")) return parse_for();
    if (reserved("case")) return parse_case();
    if (reserved("function")) return parse_function();
    if (reserved("coproc")) return parse_coproc();

    // Reserved terminators here mean a misplaced keyword.
    if (reserved_in({"then", "do", "done", "fi", "elif", "else", "esac", "}"})) {
      fail(std::string("unexpected `") + cur().text + "'");
      return nullptr;
    }

    // name () { ... }
    if (cur().type == Tok::Word && !cur().quoted && is_name(cur().text) &&
        peek(1).type == Tok::Lparen && peek(2).type == Tok::Rparen)
      return parse_funcdef_paren();

    (void)stops;
    return parse_simple();
  }

  CommandPtr parse_simple() {
    auto sc = std::make_unique<SimpleCommand>();
    bool got = false;
    bool still_prefix = true;  // leading assignment words
    while (!err) {
      if (at_redirect()) {
        parse_redirect(sc->redirects);
        got = true;
        continue;
      }
      if (cur().type == Tok::Word) {
        int wf = cur().quoted ? W_QUOTED : 0;
        if (still_prefix && is_name_assignment(cur().text))
          wf |= W_ASSIGNMENT;
        else
          still_prefix = false;
        sc->words.push_back(Word{cur().text, wf});
        advance();
        got = true;
        continue;
      }
      break;
    }
    if (!got) {
      if (is(Tok::Eof)) incomplete = true;
      fail("expected a command");
    }
    return sc;
  }

  // name= / name+= / name[subscript]= / name[subscript]+=
  static bool is_name_assignment(const std::string &s) {
    std::size_t i = 0;
    if (i >= s.size() || !(std::isalpha(static_cast<unsigned char>(s[i])) || s[i] == '_'))
      return false;
    while (i < s.size() && (std::isalnum(static_cast<unsigned char>(s[i])) || s[i] == '_')) i++;
    if (i < s.size() && s[i] == '[') {  // subscript
      int depth = 0;
      for (; i < s.size(); i++) {
        if (s[i] == '[') depth++;
        else if (s[i] == ']') { i++; break; }
      }
      (void)depth;
    }
    if (i < s.size() && s[i] == '+') i++;
    return i < s.size() && s[i] == '=';
  }

  CommandPtr parse_subshell() {
    expect(Tok::Lparen, "(");
    auto s = std::make_unique<Subshell>();
    s->body = parse_list({});
    expect(Tok::Rparen, ")");
    parse_redirect_list(s->redirects);
    return s;
  }

  CommandPtr parse_group() {
    expect_reserved("{");
    auto g = std::make_unique<Group>();
    g->body = parse_list({"}"});
    expect_reserved("}");
    parse_redirect_list(g->redirects);
    return g;
  }

  CommandPtr parse_if() {
    expect_reserved("if");
    CommandPtr node = parse_if_arm();
    expect_reserved("fi");
    if (auto *ic = dynamic_cast<IfCommand *>(node.get())) parse_redirect_list(ic->redirects);
    return node;
  }

  CommandPtr parse_if_arm() {
    auto n = std::make_unique<IfCommand>();
    n->cond = parse_list({"then"});
    expect_reserved("then");
    n->then_part = parse_list({"elif", "else", "fi"});
    if (reserved("elif")) {
      advance();
      n->else_part = parse_if_arm();
    } else if (reserved("else")) {
      advance();
      n->else_part = parse_list({"fi"});
    }
    return n;
  }

  CommandPtr parse_loop() {
    bool until = reserved("until");
    advance();  // while/until
    auto n = std::make_unique<LoopCommand>();
    n->until = until;
    n->cond = parse_list({"do"});
    expect_reserved("do");
    n->body = parse_list({"done"});
    expect_reserved("done");
    parse_redirect_list(n->redirects);
    return n;
  }

  CommandPtr parse_for() {
    bool select = reserved("select");
    advance();  // for / select
    auto n = std::make_unique<ForCommand>();
    n->is_select = select;

    if (!select && is(Tok::Lparen) && cur().glued && peek(1).type == Tok::Lparen) {
      parse_arith_for_header(*n);
    } else {
      if (cur().type != Tok::Word) {
        fail("expected variable name after `for'");
        return n;
      }
      n->var = cur().text;
      advance();
      if (reserved("in")) {
        advance();
        n->words_present = true;
        while (cur().type == Tok::Word && !reserved_in({"do"})) {
          n->words.push_back(Word{cur().text, cur().quoted ? W_QUOTED : 0});
          advance();
        }
      }
    }
    if (is(Tok::Semi) || is(Tok::Newline)) {
      advance();
      newline_list();
    }
    expect_reserved("do");
    n->body = parse_list({"done"});
    expect_reserved("done");
    parse_redirect_list(n->redirects);
    return n;
  }

  void parse_arith_for_header(ForCommand &n) {
    n.is_arith = true;
    advance();  // (
    advance();  // (
    int depth = 0;
    int part = 0;
    std::string parts[3];
    while (!is(Tok::Eof) && !err) {
      if (is(Tok::Rparen) && depth == 0) {
        if (peek(1).type == Tok::Rparen) {
          advance();
          advance();
          break;
        }
        fail("bad arithmetic `for'");
        break;
      }
      if (is(Tok::Lparen)) {
        depth++;
        parts[part] += '(';
        advance();
        continue;
      }
      if (is(Tok::Rparen)) {
        depth--;
        parts[part] += ')';
        advance();
        continue;
      }
      if (is(Tok::Semi) && depth == 0) {
        if (part < 2) part++;
        advance();
        continue;
      }
      if (!parts[part].empty()) parts[part] += ' ';
      parts[part] += tok_to_text(cur());
      advance();
    }
    n.a_init = trim(parts[0]);
    n.a_cond = trim(parts[1]);
    n.a_update = trim(parts[2]);
  }

  CommandPtr parse_arith_command() {
    std::size_t save = i;
    advance();  // (
    advance();  // (
    int depth = 0;
    std::string expr;
    bool okexpr = true;
    for (;;) {
      if (is(Tok::Eof) || is(Tok::Semi) || is(Tok::Newline)) {
        okexpr = false;  // not an arithmetic expression -> nested subshell
        break;
      }
      if (is(Tok::Rparen) && depth == 0) {
        if (peek(1).type == Tok::Rparen) {
          advance();
          advance();
          break;
        }
        okexpr = false;
        break;
      }
      if (is(Tok::Lparen)) {
        depth++;
        expr += '(';
        advance();
        continue;
      }
      if (is(Tok::Rparen)) {
        depth--;
        expr += ')';
        advance();
        continue;
      }
      if (!expr.empty()) expr += ' ';
      expr += tok_to_text(cur());
      advance();
    }
    if (!okexpr) {
      i = save;
      return parse_subshell();
    }
    auto n = std::make_unique<ArithCommand>();
    n->expression = trim(expr);
    parse_redirect_list(n->redirects);
    return n;
  }

  bool at_cond_end() const { return reserved("]]"); }

  void cond_primary(std::string &e) {
    if (err) return;
    if (is(Tok::Lparen)) {
      e += "( ";
      advance();
      cond_or(e);
      if (is(Tok::Rparen)) {
        e += " )";
        advance();
      } else {
        fail("expected `)' in conditional");
      }
      return;
    }
    if (cur().type != Tok::Word || at_cond_end()) {
      fail("expected conditional expression");
      return;
    }
    std::string w1 = cur().text;
    if (is_cond_unary(w1)) {
      advance();
      if (cur().type != Tok::Word || at_cond_end()) {
        fail("expected operand after unary operator");
        return;
      }
      e += w1;
      e += ' ';
      e += cur().text;
      advance();
      return;
    }
    e += w1;
    advance();
    bool binop = is(Tok::Less) || is(Tok::Great) ||
                 (cur().type == Tok::Word && is_cond_binop_word(cur().text));
    if (binop) {
      std::string op = (is(Tok::Less) || is(Tok::Great)) ? tok_name(cur().type) : cur().text;
      advance();
      if (cur().type != Tok::Word || at_cond_end()) {
        fail("expected operand after operator");
        return;
      }
      e += ' ';
      e += op;
      e += ' ';
      e += cur().text;
      advance();
    }
  }

  void cond_not(std::string &e) {
    if (reserved("!")) {
      e += "! ";
      advance();
      cond_not(e);
      return;
    }
    cond_primary(e);
  }

  void cond_and(std::string &e) {
    cond_not(e);
    while (!err && is(Tok::AndAnd)) {
      e += " && ";
      advance();
      newline_list();
      cond_not(e);
    }
  }

  void cond_or(std::string &e) {
    cond_and(e);
    while (!err && is(Tok::OrOr)) {
      e += " || ";
      advance();
      newline_list();
      cond_and(e);
    }
  }

  CommandPtr parse_cond() {
    advance();  // [[
    auto n = std::make_unique<CondCommand>();
    std::string expr;
    cond_or(expr);
    expect_reserved("]]");
    n->expression = trim(expr);
    parse_redirect_list(n->redirects);
    return n;
  }

  bool at_compound_start() const {
    if (is(Tok::Lparen)) return true;
    return cur().type == Tok::Word && !cur().quoted && is_compound_kw(cur().text);
  }

  CommandPtr parse_coproc() {
    advance();  // coproc
    auto n = std::make_unique<CoprocCommand>();
    bool name_then_cmd =
        cur().type == Tok::Word && !cur().quoted && is_name(cur().text) &&
        (peek(1).type == Tok::Lparen ||
         (peek(1).type == Tok::Word && !peek(1).quoted && is_compound_kw(peek(1).text)));
    if (name_then_cmd) {
      n->name = cur().text;
      advance();
      n->body = parse_command({});
    } else if (at_compound_start()) {
      n->body = parse_command({});
    } else {
      n->body = parse_simple();
    }
    return n;
  }

  CommandPtr parse_case() {
    expect_reserved("case");
    auto n = std::make_unique<CaseCommand>();
    if (cur().type != Tok::Word) {
      fail("expected word after `case'");
      return n;
    }
    n->word = Word{cur().text, cur().quoted ? W_QUOTED : 0};
    advance();
    expect_reserved("in");
    newline_list();
    while (!err && !reserved("esac")) {
      CaseClause clause;
      if (is(Tok::Lparen)) advance();  // optional leading (
      // pattern list: word ( '|' word )* ')'
      for (;;) {
        if (cur().type != Tok::Word) {
          fail("expected pattern in case");
          break;
        }
        clause.patterns.push_back(Word{cur().text, cur().quoted ? W_QUOTED : 0});
        advance();
        if (is(Tok::Pipe)) {
          advance();
          continue;
        }
        break;
      }
      expect(Tok::Rparen, ")");
      clause.body = parse_list({"esac"});
      if (is(Tok::SemiSemi)) {
        clause.terminator = 0;
        advance();
      } else if (is(Tok::SemiAnd)) {
        clause.terminator = 1;
        advance();
      } else if (is(Tok::SemiSemiAnd)) {
        clause.terminator = 2;
        advance();
      }
      newline_list();
      n->clauses.push_back(std::move(clause));
    }
    expect_reserved("esac");
    parse_redirect_list(n->redirects);
    return n;
  }

  CommandPtr parse_funcdef_paren() {
    auto n = std::make_unique<FunctionDef>();
    n->name = cur().text;
    advance();  // name
    expect(Tok::Lparen, "(");
    expect(Tok::Rparen, ")");
    newline_list();
    n->body = parse_command({});
    return n;
  }

  CommandPtr parse_function() {
    expect_reserved("function");
    auto n = std::make_unique<FunctionDef>();
    if (cur().type != Tok::Word) {
      fail("expected function name");
      return n;
    }
    n->name = cur().text;
    advance();
    if (is(Tok::Lparen)) {
      advance();
      expect(Tok::Rparen, ")");
    }
    newline_list();
    n->body = parse_command({});
    return n;
  }

  ParseResult run() {
    ParseResult res;
    if (!toks.empty() && toks.back().lex_error) {
      res.ok = false;
      res.incomplete = true;  // unterminated quote/substitution/here-doc
      res.error = "unterminated quoted string or substitution";
      return res;
    }
    newline_list();
    if (is(Tok::Eof)) return res;
    res.command = parse_list({});
    newline_list();
    if (!err && !is(Tok::Eof))
      fail(std::string("unexpected token `") + tok_name(cur().type) + "'");
    if (err) {
      res.ok = false;
      res.error = errmsg;
      res.incomplete = incomplete;
      res.command.reset();
    }
    return res;
  }
};

}  // namespace

ParseResult parse(const std::string &input) {
  Parser p(tokenize(input));
  return p.run();
}

}  // namespace gnash::core
