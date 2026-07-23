// Copyright (c) 2026 Brian J. Fox
// Licensed under GPLv2 with the GPLv2-AI Exception.

// parser.cpp -- recursive-descent parser (see parser.hpp).

#include "gnash/core/parser.hpp"
#include "gnash/core/subscript.hpp"

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <map>
#include <set>
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

// A word usable as a function name in the `name ()' form.  bash permits far
// more than POSIX identifiers here (e.g. `ns::fn', `verify-git-version').  The
// lexer already guarantees an unquoted WORD contains no shell metacharacters,
// so only reject characters that would make it an assignment or an expansion.
bool is_funcname(const std::string &s) {
  if (s.empty()) return false;
  if (is_name(s)) return true;
  for (char c : s)
    if (c == '=' || c == '$' || c == '`' || c == '\'' || c == '"' || c == '\\')
      return false;
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
  bool assign_error = false;  // compound-assignment syntax error (`a=(x & y)')
  std::string errmsg;
  int err_line = 0;  // source line of the first failure
  // Stack of open compound commands (opener word/char + its line), so an EOF
  // before the matching closer reports "unexpected end of file from `X'
  // command on line N", as bash does.
  std::vector<std::pair<std::string, int>> open_cmds;
  void push_open(const std::string &w) { open_cmds.emplace_back(w, cur().line); }
  void pop_open() { if (!open_cmds.empty()) open_cmds.pop_back(); }
  // At EOF with an open compound, produce bash's grammar-level message.
  bool eof_from_open() {
    if (!is(Tok::Eof) || open_cmds.empty()) return false;
    incomplete = true;
    fail("unexpected end of file from `" + open_cmds.back().first +
         "' command on line " + std::to_string(open_cmds.back().second));
    return true;
  }

  explicit Parser(std::vector<Token> t) : toks(std::move(t)) {}

  // Recursion-depth guard: deeply nested constructs -- e.g. thousands of nested
  // `(...)' subshells, `if's, or `[[ (...) ]]' -- would otherwise overflow the
  // C++ call stack and crash the process.  The cap is far above any real script
  // (bash tolerates a few thousand levels before it, too, segfaults); on excess
  // we report a syntax error and unwind cleanly instead of faulting.
  std::size_t nest_depth = 0;
  static constexpr std::size_t kMaxNesting = 1000;
  struct NestGuard {
    Parser &p;
    bool ok;
    explicit NestGuard(Parser &pp) : p(pp) {
      ok = (++p.nest_depth <= kMaxNesting);
      if (!ok) p.fail("maximum nesting depth exceeded");
    }
    ~NestGuard() { --p.nest_depth; }
  };

  const Token &cur() const { return toks[i]; }
  const Token &peek(std::size_t k) const {
    std::size_t j = i + k;
    return j < toks.size() ? toks[j] : toks.back();
  }
  void advance() {
    if (i + 1 < toks.size()) i++;
  }
  bool is(Tok t) const { return cur().type == t; }

  // Literal source spelling of a token (for reconstructing a =~ regex whose
  // metacharacters were tokenized as operators).
  static std::string tok_source(const Token &t) {
    switch (t.type) {
      case Tok::Word: case Tok::IoNumber: return t.text;
      case Tok::Amp: return "&";       case Tok::Semi: return ";";
      case Tok::Pipe: return "|";      case Tok::AndAnd: return "&&";
      case Tok::OrOr: return "||";     case Tok::Lparen: return "(";
      case Tok::Rparen: return ")";    case Tok::Less: return "<";
      case Tok::Great: return ">";     case Tok::SemiSemi: return ";;";
      case Tok::PipeAnd: return "|&";
      default: return t.text;
    }
  }

  // Encode a reconstructed =~ regex so it survives re-tokenization and quote
  // removal in the conditional evaluator: existing backslashes are doubled
  // (quote removal halves them again) and characters the lexer treats as
  // operators/separators are backslash-escaped.  `$' is left alone so
  // variables in the pattern still expand.
  static std::string encode_regex(const std::string &rx) {
    std::string e;
    for (char c : rx) {
      if (c == '\\') { e += "\\\\"; continue; }
      if (std::strchr("()|&;<> \t", c)) e += '\\';
      e += c;
    }
    return e;
  }

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
      err_line = i < toks.size() ? toks[i].line : 1;
    }
  }
  void expect_reserved(const char *w) {
    if (reserved(w))
      advance();
    else {
      if (eof_from_open()) return;
      if (is(Tok::Eof)) incomplete = true;
      fail(std::string("expected `") + w + "'");
    }
  }
  void expect(Tok t, const char *name) {
    if (is(t))
      advance();
    else {
      if (eof_from_open()) return;
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

  // `&' backgrounds only the command immediately before it.  Because the list
  // is built left-associatively, that command is the rightmost leaf reached by
  // descending LEFT->second through list-level connectors (`;', newline, or an
  // earlier `&').  Recursing to that leaf makes `X; A & B & wait' background A
  // and B as two separate jobs -- not `A & B' together in one child.
  static CommandPtr background_tail(CommandPtr left, CommandPtr right) {
    if (auto *cn = dynamic_cast<Connection *>(left.get());
        cn && (cn->conn == Connector::Semi || cn->conn == Connector::Newline ||
               cn->conn == Connector::Amp)) {
      cn->second = background_tail(std::move(cn->second), std::move(right));
      return left;
    }
    return connect(Connector::Amp, std::move(left), std::move(right));
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
        if (conn == Connector::Amp) left = background_tail(std::move(left), nullptr);
        break;
      }
      CommandPtr right = parse_and_or(stops);
      if (conn == Connector::Amp)
        left = background_tail(std::move(left), std::move(right));
      else
        left = connect(conn, std::move(left), std::move(right));
    }
    return left;
  }

  // Like parse_list, but bash requires at least one command here (an if/while/
  // until/for condition or body): an empty list is the syntax error `near
  // unexpected token <stop>', naming the reserved word that appears where a
  // command was expected (then/do/done/fi/...).
  CommandPtr parse_required_list(std::initializer_list<const char *> stops) {
    CommandPtr c = parse_list(stops);
    if (!c && !err)
      fail(is(Tok::Eof) ? std::string("unexpected end of file")
                        : std::string("near unexpected token `") + tok_to_text(cur()) + "'");
    return c;
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
        // `time' accepts `-p' (POSIX output) and `--' (end of options), in that
        // order, before the pipeline.
        while (true) {
          if (reserved("-p")) { cmdflags |= CMD_TIME_POSIX; advance(); continue; }
          if (reserved("--")) { advance(); break; }
          break;
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
    NestGuard g(*this);
    if (!g.ok) return nullptr;
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
      fail(is(Tok::Eof) ? std::string("unexpected end of file")
                        : std::string("near unexpected token `") + tok_to_text(cur()) + "'");
      return nullptr;
    }

    // name () { ... }
    if (cur().type == Tok::Word && !cur().quoted && is_funcname(cur().text) &&
        peek(1).type == Tok::Lparen && peek(2).type == Tok::Rparen)
      return parse_funcdef_paren();

    (void)stops;
    return parse_simple();
  }

  CommandPtr parse_simple() {
    auto sc = std::make_unique<SimpleCommand>();
    sc->line = cur().line;  // $LINENO of this command
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
        if (still_prefix && is_name_assignment(cur().text)) {
          std::string bad = array_literal_bad_token(cur().text);
          if (!bad.empty()) {  // e.g. `a=(x & y)': invalid in a compound assignment
            fail("near unexpected token `" + bad + "'");
            assign_error = true;  // bash reports $?=1 for this, not the usual 2
            return sc;
          }
          wf |= W_ASSIGNMENT;
        } else
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
    if (i < s.size() && s[i] == '[') {  // subscript (quote/escape aware)
      std::size_t close = skip_subscript(s, i);
      if (close == std::string::npos) return false;
      i = close + 1;
    }
    if (i < s.size() && s[i] == '+') i++;
    return i < s.size() && s[i] == '=';
  }

  // If W is an array-literal assignment `name=(...)' whose contents contain a
  // token that is invalid there -- a control operator (`&' `|' `;' `&&' `||'),
  // a redirection, or `(' -- return that token's spelling; else "".  bash
  // accepts only words, `[sub]=' elements, and newlines inside `(...)'.
  static std::string array_literal_bad_token(const std::string &w) {
    std::size_t i = 0;
    if (i >= w.size() || !(std::isalpha(static_cast<unsigned char>(w[i])) || w[i] == '_'))
      return "";
    while (i < w.size() && (std::isalnum(static_cast<unsigned char>(w[i])) || w[i] == '_')) i++;
    if (i < w.size() && w[i] == '[') {
      std::size_t close = skip_subscript(w, i);
      if (close == std::string::npos) return "";
      i = close + 1;
    }
    if (i < w.size() && w[i] == '+') i++;
    if (i >= w.size() || w[i] != '=') return "";
    i++;  // past `='
    if (i >= w.size() || w[i] != '(' || w.back() != ')') return "";  // not an array literal
    for (const Token &t : tokenize(w.substr(i + 1, w.size() - i - 2))) {
      if (t.type == Tok::Eof || t.type == Tok::Word || t.type == Tok::Newline ||
          t.type == Tok::IoNumber)
        continue;
      return tok_to_text(t);
    }
    return "";
  }

  CommandPtr parse_subshell() {
    push_open("(");
    expect(Tok::Lparen, "(");
    auto s = std::make_unique<Subshell>();
    s->body = parse_list({});
    expect(Tok::Rparen, ")");
    pop_open();
    parse_redirect_list(s->redirects);
    return s;
  }

  CommandPtr parse_group() {
    push_open("{");
    expect_reserved("{");
    auto g = std::make_unique<Group>();
    g->body = parse_list({"}"});
    expect_reserved("}");
    pop_open();
    parse_redirect_list(g->redirects);
    return g;
  }

  CommandPtr parse_if() {
    push_open("if");
    expect_reserved("if");
    CommandPtr node = parse_if_arm();
    expect_reserved("fi");
    pop_open();
    if (auto *ic = dynamic_cast<IfCommand *>(node.get())) parse_redirect_list(ic->redirects);
    return node;
  }

  CommandPtr parse_if_arm() {
    auto n = std::make_unique<IfCommand>();
    n->cond = parse_required_list({"then"});
    expect_reserved("then");
    n->then_part = parse_required_list({"elif", "else", "fi"});
    if (reserved("elif")) {
      advance();
      n->else_part = parse_if_arm();
    } else if (reserved("else")) {
      advance();
      n->else_part = parse_required_list({"fi"});
    }
    return n;
  }

  CommandPtr parse_loop() {
    bool until = reserved("until");
    push_open(until ? "until" : "while");
    advance();  // while/until
    auto n = std::make_unique<LoopCommand>();
    n->until = until;
    n->cond = parse_required_list({"do"});
    expect_reserved("do");
    n->body = parse_required_list({"done"});
    expect_reserved("done");
    pop_open();
    parse_redirect_list(n->redirects);
    return n;
  }

  CommandPtr parse_for() {
    bool select = reserved("select");
    int for_line = cur().line;  // $LINENO of the `for'/`select' keyword
    push_open(select ? "select" : "for");
    advance();  // for / select
    auto n = std::make_unique<ForCommand>();
    n->is_select = select;
    n->line = for_line;

    if (!select && is(Tok::Lparen) && cur().glued && peek(1).type == Tok::Lparen) {
      parse_arith_for_header(*n);
    } else {
      if (cur().type != Tok::Word) {
        fail(is(Tok::Eof) ? std::string("unexpected end of file")
                          : std::string("near unexpected token `") + tok_to_text(cur()) + "'");
        return n;
      }
      n->var = cur().text;
      advance();
      if (is(Tok::Semi)) {
        advance();
        newline_list();
      } else {
        newline_list();  // POSIX allows a linebreak before `in'
        if (reserved("in")) {
          advance();
          n->words_present = true;
          while (cur().type == Tok::Word && !reserved_in({"do"})) {
            n->words.push_back(Word{cur().text, cur().quoted ? W_QUOTED : 0});
            advance();
          }
        }
      }
    }
    if (is(Tok::Semi) || is(Tok::Newline)) {
      advance();
      newline_list();
    }
    if (reserved("{")) {  // bash allows a brace group as the body
      n->body = parse_command({});
      parse_redirect_list(n->redirects);
      return n;
    }
    expect_reserved("do");
    n->body = parse_required_list({"done"});
    expect_reserved("done");
    pop_open();
    parse_redirect_list(n->redirects);
    return n;
  }

  void parse_arith_for_header(ForCommand &n) {
    n.is_arith = true;
    advance();  // (
    advance();  // (
    int depth = 0;
    std::vector<std::string> parts(1);
    while (!is(Tok::Eof) && !err) {
      if (is(Tok::Rparen) && depth == 0) {
        if (peek(1).type == Tok::Rparen) {
          // Keep a blank before `))' so the reconstructed step and any error
          // diagnostic match bash's raw text (`for ((...; i++ ))').
          if (!parts.back().empty() && cur().preceded_by_blank) parts.back() += ' ';
          advance();
          advance();
          break;
        }
        fail("bad arithmetic `for'");
        break;
      }
      if (is(Tok::Lparen)) {
        depth++;
        parts.back() += '(';
        advance();
        continue;
      }
      if (is(Tok::Rparen)) {
        depth--;
        parts.back() += ')';
        advance();
        continue;
      }
      if (is(Tok::Semi) && depth == 0) {
        parts.emplace_back();
        advance();
        continue;
      }
      // `;;' lexes as a single token, but between the arith-for parentheses
      // two adjacent semicolons just delimit an empty middle section
      // (`for (( ;; ))', `for (( i=0;;i++ ))'), so split into two parts.
      if (is(Tok::SemiSemi) && depth == 0) {
        parts.emplace_back();
        parts.emplace_back();
        advance();
        continue;
      }
      if (!parts.back().empty() && cur().preceded_by_blank) parts.back() += ' ';
      parts.back() += tok_to_text(cur());
      advance();
    }
    // Exactly three expressions, as bash requires; the diagnostics are two
    // lines (both get the `syntax error: ' prefix when printed).
    if (!err && parts.size() != 3) {
      std::string raw = "(( ";
      for (size_t k = 0; k < parts.size(); k++) {
        if (k) raw += "; ";
        raw += trim(parts[k]);
      }
      raw += " ))";
      fail(std::string(parts.size() < 3 ? "arithmetic expression required"
                                        : "`;' unexpected") +
           "\n`" + raw + "'");
      return;
    }
    // Keep any trailing blank on the step (from the blank before `))') so its
    // display and error text match bash; no section ever gets a leading blank.
    n.a_init = parts[0];
    n.a_cond = parts[1];
    n.a_update = parts[2];
  }

  CommandPtr parse_arith_command() {
    std::size_t save = i;
    int arith_line = cur().line;  // $LINENO / error line of the `((' command
    advance();  // (
    advance();  // (
    int depth = 0;
    std::string expr;
    bool okexpr = true;
    for (;;) {
      if (is(Tok::Eof)) {  // bash reports the unbalanced (( at EOF directly
        incomplete = true;
        fail("unexpected EOF while looking for matching `)'");
        return nullptr;
      }
      if (is(Tok::Semi) || is(Tok::Newline)) {
        okexpr = false;  // not an arithmetic expression -> nested subshell
        break;
      }
      if (is(Tok::Rparen) && depth == 0) {
        if (peek(1).type == Tok::Rparen) {
          // Keep a blank before `))' so an arithmetic error diagnostic
          // reproduces bash's raw expression (`((: 7++ : ...', token `"+ "');
          // the single-line printer trims it back off for display.
          if (!expr.empty() && cur().preceded_by_blank) expr += ' ';
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
      // Join tokens without inserting spaces: the lexer may split a compound
      // arithmetic operator such as `>=' into a redirection token (`>') plus a
      // word (`=3'); re-gluing them reconstructs the original operator, while a
      // space ("> =3") would make it unparseable.  A blank in the source still
      // separates operands, so distinct operands keep their own tokens.
      if (!expr.empty() && cur().preceded_by_blank) expr += ' ';
      expr += tok_to_text(cur());
      advance();
    }
    if (!okexpr) {
      i = save;
      return parse_subshell();
    }
    auto n = std::make_unique<ArithCommand>();
    // Keep the trailing blank (the reconstruction never yields a leading one)
    // so an error diagnostic matches bash's raw expression text.
    n->expression = expr;
    n->line = arith_line;
    parse_redirect_list(n->redirects);
    return n;
  }

  bool at_cond_end() const { return reserved("]]"); }

  void cond_primary(std::string &e) {
    if (err) return;
    NestGuard g(*this);
    if (!g.ok) return;
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
      if (op == "=~") {
        // The right side is an extended regular expression: reassemble it from
        // the original source, gluing tokens that were not separated by
        // whitespace (so `([0-9]+)-([0-9]+)' stays one pattern).
        if (at_cond_end() || is(Tok::Eof)) {
          fail("expected operand after operator");
          return;
        }
        std::string rx = tok_source(cur());
        advance();
        while (!err && !at_cond_end() && !is(Tok::Eof) && !cur().preceded_by_blank) {
          rx += tok_source(cur());
          advance();
        }
        e += " =~ ";
        e += encode_regex(rx);
        return;
      }
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
    NestGuard g(*this);
    if (!g.ok) return;
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
    int cond_line = cur().line;  // $LINENO / error line of the `[[' command
    advance();  // [[
    auto n = std::make_unique<CondCommand>();
    std::string expr;
    cond_or(expr);
    expect_reserved("]]");
    n->expression = trim(expr);
    n->line = cond_line;
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
    int case_line = cur().line;  // $LINENO / error line of the case command
    push_open("case");
    expect_reserved("case");
    auto n = std::make_unique<CaseCommand>();
    n->line = case_line;
    if (cur().type != Tok::Word) {
      fail("expected word after `case'");
      return n;
    }
    n->word = Word{cur().text, cur().quoted ? W_QUOTED : 0};
    advance();
    newline_list();  // POSIX allows a linebreak before `in'
    if (reserved("in")) {
      advance();
    } else {
      if (is(Tok::Eof)) incomplete = true;  // more input may supply `in'
      fail(is(Tok::Eof) ? std::string("unexpected end of file")
                        : std::string("near unexpected token `") + tok_to_text(cur()) + "'");
      return n;
    }
    newline_list();
    while (!err && !reserved("esac")) {
      CaseClause clause;
      if (is(Tok::Lparen)) advance();  // optional leading (
      // pattern list: word ( '|' word )* ')'
      for (;;) {
        if (cur().type != Tok::Word) {
          if (!eof_from_open()) fail("expected pattern in case");
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
    pop_open();
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
    // bash validates the function name at the end of the definition, so an
    // invalid-name error reports the line the definition closes on.
    n->line = i > 0 ? toks[i - 1].line : cur().line;
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
    n->line = i > 0 ? toks[i - 1].line : cur().line;  // the definition's end line
    return n;
  }

  ParseResult run() {
    ParseResult res;
    if (!toks.empty() && toks.back().lex_error) {
      res.ok = false;
      res.incomplete = true;  // unterminated quote/substitution
      char cl = toks.back().lex_close;
      res.error = cl ? std::string("unexpected EOF while looking for matching `") + cl + "'"
                     : std::string("unterminated quoted string or substitution");
      res.error_line = toks.back().line;
      return res;
    }
    newline_list();
    if (is(Tok::Eof)) return res;
    res.command = parse_list({});
    newline_list();
    if (!err && !is(Tok::Eof))
      fail(is(Tok::Eof) ? std::string("unexpected end of file")
                        : std::string("near unexpected token `") + tok_to_text(cur()) + "'");
    if (err) {
      res.ok = false;
      res.error = errmsg;
      res.error_line = err_line;
      res.incomplete = incomplete;
      res.assign_error = assign_error;
      res.command.reset();
      return res;
    }
    // A here-document delimited by end of input: runnable, but incomplete for
    // callers that can supply more lines.
    if (!toks.empty() && toks.back().heredoc_eof) {
      res.incomplete = true;
      res.heredoc_eof = true;
      res.heredoc_eof_delim = toks.back().heredoc_eof_delim;
      res.heredoc_eof_line = toks.back().heredoc_eof_line;
      res.heredoc_eof_quoted = toks.back().heredoc_eof_quoted;
    }
    return res;
  }
};

}  // namespace

// Expand aliases in command position on a token stream (in place).  An
// unquoted word in command position that names an alias is replaced by the
// alias body's tokens; a body ending in blank makes the following word eligible
// too, and a word is not re-expanded within its own expansion.
static void expand_alias_tokens(std::vector<Token> &toks,
                                const std::map<std::string, std::string> &aliases,
                                const std::map<std::string, std::string> &global_aliases,
                                const std::map<std::string, std::string> &suffix_aliases) {
  if (aliases.empty() && global_aliases.empty() && suffix_aliases.empty()) return;
  static const std::set<std::string> kw = {
      "if", "then", "else", "elif", "do", "done", "{", "}", "while", "until",
      "for", "case", "select", "fi", "esac", "!", "time", "function"};
  bool cmd_pos = true, next_also = false;
  // After `for'/`select'/`case' the next word is a variable name (or the value
  // being matched), not a command word, so it must NOT be alias-expanded -- e.g.
  // `for j in ...; do' with `alias j=...' must keep `j' literal, as bash does.
  bool name_next = false;
  std::set<std::string> active;
  int guard = 0;
  for (size_t i = 0; i < toks.size() && guard < 10000;) {
    Tok ty = toks[i].type;
    if (ty == Tok::Newline || ty == Tok::Semi || ty == Tok::Amp || ty == Tok::Pipe ||
        ty == Tok::AndAnd || ty == Tok::OrOr || ty == Tok::Lparen || ty == Tok::SemiSemi) {
      cmd_pos = true; next_also = false; name_next = false; active.clear(); i++; continue;
    }
    if (ty != Tok::Word) { i++; continue; }
    const std::string text = toks[i].text;
    bool quoted = toks[i].quoted;
    if (!quoted && kw.count(text)) {
      cmd_pos = true; next_also = false;
      name_next = (text == "for" || text == "select" || text == "case");
      i++; continue;
    }
    // The variable name after for/select/case: never an alias, and it is not a
    // command word, so what follows it (the `in'/`do'/list) is not either.
    if (name_next) { name_next = false; cmd_pos = false; i++; continue; }
    // zsh global aliases (`alias -g') expand in ANY word position.
    if (!quoted && global_aliases.count(text) && !active.count(text)) {
      active.insert(text);
      guard++;
      std::vector<Token> rep = tokenize(global_aliases.at(text));
      if (!rep.empty() && rep.back().type == Tok::Eof) rep.pop_back();
      toks.erase(toks.begin() + static_cast<long>(i));
      toks.insert(toks.begin() + static_cast<long>(i), rep.begin(), rep.end());
      continue;  // reprocess from i: the body may contain operators (| > ...)
    }
    // zsh suffix aliases (`alias -s ext=cmd'): a bare `file.ext' in command
    // position, whose `ext' has a suffix alias and which is not itself an alias,
    // runs `cmd file.ext'.
    if ((cmd_pos || next_also) && !quoted && !suffix_aliases.empty() && !aliases.count(text)) {
      size_t dot = text.rfind('.');
      if (dot != std::string::npos && dot + 1 < text.size()) {
        auto sit = suffix_aliases.find(text.substr(dot + 1));
        if (sit != suffix_aliases.end()) {
          guard++;
          std::vector<Token> cmd = tokenize(sit->second);
          if (!cmd.empty() && cmd.back().type == Tok::Eof) cmd.pop_back();
          toks.insert(toks.begin() + static_cast<long>(i), cmd.begin(), cmd.end());
          continue;  // reprocess from i: the inserted command is now cmd_pos
        }
      }
    }
    if ((cmd_pos || next_also) && !quoted && aliases.count(text) && !active.count(text)) {
      const std::string &val = aliases.at(text);
      active.insert(text);
      // Alias expansion is a textual substitution: the replacement tokens all
      // report the line where the alias was invoked, even when the body itself
      // contains newlines (bash does not advance $LINENO across an alias body).
      int inv_line = toks[i].line;
      bool trailing = !val.empty() && (val.back() == ' ' || val.back() == '\t');
      std::vector<Token> rep = tokenize(val);
      if (!rep.empty() && rep.back().type == Tok::Eof) rep.pop_back();
      for (Token &t : rep) t.line = inv_line;
      toks.erase(toks.begin() + static_cast<long>(i));
      toks.insert(toks.begin() + static_cast<long>(i), rep.begin(), rep.end());
      guard++;
      // If the body ends in a blank, the word that followed the alias is also
      // eligible for expansion (bash chains this while each body ends blank).
      size_t np = i + rep.size();
      while (trailing && np < toks.size() && toks[np].type == Tok::Word && !toks[np].quoted &&
             aliases.count(toks[np].text) && !active.count(toks[np].text) && guard < 10000) {
        const std::string &v2 = aliases.at(toks[np].text);
        active.insert(toks[np].text);
        trailing = !v2.empty() && (v2.back() == ' ' || v2.back() == '\t');
        int np_line = toks[np].line;
        std::vector<Token> r2 = tokenize(v2);
        if (!r2.empty() && r2.back().type == Tok::Eof) r2.pop_back();
        for (Token &t : r2) t.line = np_line;
        toks.erase(toks.begin() + static_cast<long>(np));
        toks.insert(toks.begin() + static_cast<long>(np), r2.begin(), r2.end());
        guard++;
        np += r2.size();
      }
      next_also = false;
      continue;  // reprocess from i (its command word may itself be an alias)
    }
    cmd_pos = false; next_also = false; i++;
  }
}

ParseResult parse(const std::string &input) {
  Parser p(tokenize(input));
  return p.run();
}

ParseResult parse_with_aliases(const std::string &input,
                               const std::map<std::string, std::string> &aliases,
                               const std::map<std::string, std::string> &global_aliases,
                               const std::map<std::string, std::string> &suffix_aliases) {
  std::vector<Token> toks = tokenize(input);
  expand_alias_tokens(toks, aliases, global_aliases, suffix_aliases);
  Parser p(std::move(toks));
  return p.run();
}

}  // namespace gnash::core
