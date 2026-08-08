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

// bash's test_unop(): the exact set, not "a dash and any letter".  `-Q' is not
// one, so `[[ -Q 7 ]]' reads `-Q' as the left operand and then complains that
// `7' is not a binary operator.
bool is_cond_unary(const std::string &w) {
  return w.size() == 2 && w[0] == '-' && w[1] != '\0' &&
         std::strchr("abcdefghknoprstuvwxzGLOSNR", w[1]) != nullptr;
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

  // Encode a reconstructed =~ regex so it survives re-tokenization in the
  // conditional evaluator.  A user's backslashes are left exactly as written,
  // so the expander sees what they quoted and the regex escaping can follow
  // it: `\(' marks the paren quoted, and a quoted paren matches literally.
  // Characters the lexer treats as operators or separators are replaced by a
  // COND_RX_ESC marker plus an ordinary letter, rather than being
  // backslash-escaped: a backslash here would be indistinguishable from the
  // user's own quoting once the expander records what was quoted, and quoting
  // is what decides whether a regex metacharacter matches literally.  `$' is
  // left alone so variables in the pattern still expand.
  static std::string encode_regex(const std::string &rx) {
    std::string e;
    for (char c : rx) {
      const char *k = std::strchr(kCondRxRaw, c);
      if (c != '\0' && k) {
        e += COND_RX_ESC;
        e += kCondRxSub[k - kCondRxRaw];
        continue;
      }
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
      if (is(Tok::Eof)) {
        incomplete = true;
        fail(std::string("expected `") + w + "'");
        return;
      }
      // A real token in the wrong place is bash's "near unexpected token"
      // (`for z in 1 2 3; done' -> near `done'), not a grammar expectation.
      fail(std::string("near unexpected token `") + tok_to_text(cur()) + "'");
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

  // A redirection *operator* token (not an IoNumber): what may follow a `{var}'
  // fd-variable specifier or an IoNumber.
  static bool is_redir_op(Tok t) {
    switch (t) {
      case Tok::Less: case Tok::Great: case Tok::DGreat: case Tok::DLess:
      case Tok::DLessDash: case Tok::TLess: case Tok::LessAnd: case Tok::GreatAnd:
      case Tok::LessGreat: case Tok::Clobber: case Tok::AndGreat: case Tok::AndDGreat:
        return true;
      default:
        return false;
    }
  }

  // If W is a `{name}' fd-variable specifier (`{fd}>file'), return `name', else
  // "".  Only a bare identifier in braces qualifies.
  static std::string fd_var_name(const std::string &w) {
    if (w.size() < 3 || w.front() != '{' || w.back() != '}') return "";
    std::string n = w.substr(1, w.size() - 2);
    if (!(std::isalpha(static_cast<unsigned char>(n[0])) || n[0] == '_')) return "";
    size_t i = 1;
    while (i < n.size() && (std::isalnum(static_cast<unsigned char>(n[i])) || n[i] == '_')) i++;
    // An array element is a valid target too: `exec {fd[0]}<&0' (bash).
    if (i < n.size() && n[i] == '[' && n.back() == ']') return n;
    if (i != n.size()) return "";
    return n;
  }

  bool parse_redirect(std::vector<Redirect> &redirs, const std::string &fd_var = "") {
    Redirect r;
    r.fd_var = fd_var;
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

  // An unquoted `{name}' glued to a redirection operator (`{fd}>file'); returns
  // the variable name via NAME.
  bool at_var_redirect(std::string &name) {
    if (cur().type == Tok::Word && !cur().quoted && is_redir_op(peek(1).type) &&
        !peek(1).preceded_by_blank) {
      name = fd_var_name(cur().text);
      return !name.empty();
    }
    return false;
  }

  void parse_redirect_list(std::vector<Redirect> &redirs) {
    for (;;) {
      if (!err && at_redirect()) { parse_redirect(redirs); continue; }
      std::string fv;
      if (!err && at_var_redirect(fv)) { advance(); parse_redirect(redirs, fv); continue; }
      break;
    }
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

    // name () { ... } -- a QUOTED word is still parsed as a function
    // definition (bash's grammar takes any WORD here); its invalid name is
    // reported at execution, and the reader keeps going.
    if (cur().type == Tok::Word && (cur().quoted || is_funcname(cur().text)) &&
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
      // `{var}<file': an unquoted `{name}' glued to a redirection operator names
      // the variable that receives the freshly allocated descriptor.
      {
        std::string fv;
        if (at_var_redirect(fv)) {
          advance();  // consume `{name}'
          parse_redirect(sc->redirects, fv);
          got = true;
          continue;
        }
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
    // bash's make_subshell_command() records `line_number' as it reduces
    // `( compound_list )', so a subshell's line is the line of its CLOSING
    // paren, not its opening one.  It is the only compound command whose line
    // the executor installs, so this is what a diagnostic from anywhere inside
    // the subshell reports.
    s->line = i > 0 ? toks[i - 1].line : cur().line;
    if (is(Tok::Rparen)) s->line = cur().line;
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
        // Keep any blank before the `;' on the section just closed, so a section
        // written as `EXPR ;' reproduces bash's raw text (`7=4 ') in an error.
        if (!parts.back().empty() && cur().preceded_by_blank) parts.back() += ' ';
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
      if (cur().preceded_by_blank) expr += ' ';
      expr += tok_to_text(cur());
      advance();
    }
    if (!okexpr) {
      i = save;
      return parse_subshell();
    }
    auto n = std::make_unique<ArithCommand>();
    // Keep the leading and trailing blanks so an error diagnostic and the
    // xtrace line match bash's raw expression text (`+ ((  42  ))').
    n->expression = expr;
    n->line = arith_line;
    parse_redirect_list(n->redirects);
    return n;
  }

  bool at_cond_end() const { return reserved("]]"); }

  // bash reports a conditional-expression error in three lines: a diagnostic
  // naming what went wrong, then `syntax error near `TOKEN'', then the source
  // line.  The first is emitted verbatim -- the printer leaves the conditional
  // diagnostics unadorned rather than prefixing them with `syntax error:'.
  int cond_open_line = 0;  // line of the `[[' currently being parsed
  // True when nothing but blank lines remains: bash calls that EOF, and names
  // the token `EOF' rather than the NEWLINE that happens to be sitting there.
  bool at_input_end() const {
    for (std::size_t k = i; k < toks.size(); k++) {
      if (toks[k].type == Tok::Eof) return true;
      if (toks[k].type != Tok::Newline) return false;
    }
    return true;
  }
  // At end of input bash pairs the conditional diagnostic with the
  // grammar-level report, on the following line -- and no `near'/source echo.
  void cond_eof_fail(const std::string &diag) {
    incomplete = true;
    fail(diag + "\nunexpected end of file from `[[' command on line " +
         std::to_string(cond_open_line));
  }

  void cond_fail(const std::string &diag) {
    fail(diag + "\nnear `" + tok_to_text(cur()) + "'");
  }
  // The token as bash names it in those diagnostics.
  std::string cond_tok() const { return tok_to_text(cur()); }

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
      } else if (at_input_end()) {
        cond_eof_fail("unexpected token `EOF', expected `)'");
      } else {
        cond_fail("unexpected token `" + cond_tok() + "', expected `)'");
      }
      return;
    }
    if (cur().type != Tok::Word || at_cond_end()) {
      cond_fail("unexpected token `" + cond_tok() + "' in conditional command");
      return;
    }
    std::string w1 = cur().text;
    if (is_cond_unary(w1)) {
      advance();
      if (cur().type != Tok::Word || at_cond_end()) {
        cond_fail("unexpected argument `" + cond_tok() +
                  "' to conditional unary operator");
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
          cond_fail("unexpected argument `" + cond_tok() +
                    "' to conditional binary operator");
          return;
        }
        // Whitespace normally ends the regex, but not while inside an unclosed
        // `(...)' group: bash keeps `(a b)' (and nested `(a (b c))') as one
        // pattern, and a blank or the closing `]]' only ends the regex at paren
        // depth 0.  Track `('/`)' across tokens; `[a b]' (depth 0 at the blank)
        // still ends, matching bash.
        int pd = 0;
        auto count_parens = [&pd](const std::string &s) {
          for (char c : s) {
            if (c == '(') pd++;
            else if (c == ')') pd--;
          }
        };
        std::string rx = tok_source(cur());
        count_parens(rx);
        advance();
        while (!err && !is(Tok::Eof)) {
          // At paren depth 0 the regex ends at a blank, the closing `]]', or a
          // `)' -- the last being a conditional group close (`[[ ( X =~ RE ) ]]'),
          // not regex content, exactly as bash treats a bare `)' there.
          if (pd == 0 && (at_cond_end() || cur().preceded_by_blank ||
                          cur().type == Tok::Rparen))
            break;
          if (cur().preceded_by_blank) rx += ' ';  // blank inside (...) is regex
          std::string src = tok_source(cur());
          rx += src;
          count_parens(src);
          advance();
        }
        e += " =~ ";
        e += encode_regex(rx);
        return;
      }
      if (cur().type != Tok::Word || at_cond_end()) {
        cond_fail("unexpected argument `" + cond_tok() +
                  "' to conditional binary operator");
        return;
      }
      e += ' ';
      e += op;
      e += ' ';
      e += cur().text;
      advance();
    } else if (!at_cond_end() && !is(Tok::Eof) && !is(Tok::AndAnd) &&
               !is(Tok::OrOr) && !is(Tok::Rparen)) {
      // `[[ x ]]' is `[[ -n x ]]', but only when the expression really ends
      // here: anything else in operator position is bash's complaint that it
      // wanted a binary operator (`[[ 4 & ]]', `[[ -Q 7 ]]').
      cond_fail("unexpected token `" + cond_tok() +
                "', conditional binary operator expected");
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
    cond_open_line = cond_line;
    advance();  // [[
    auto n = std::make_unique<CondCommand>();
    std::string expr;
    cond_or(expr);
    if (!err && !at_cond_end()) {
      if (at_input_end()) cond_eof_fail("unexpected EOF while looking for `]]'");
      else cond_fail("syntax error in conditional expression: unexpected token `" +
                     cond_tok() + "'");
    }
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

  // A function body must be a compound command: a brace group, subshell,
  // arithmetic/conditional command, or a loop/if/case/select.  Unlike the
  // general compound-start test, `function' and `coproc' are not valid bodies
  // (bash rejects `f() coproc ...' and `f() function g ...').
  bool at_function_body() const {
    if (is(Tok::Lparen)) return true;  // ( subshell or (( arithmetic
    return cur().type == Tok::Word && !cur().quoted &&
           cur().text != "function" && is_compound_kw(cur().text);
  }

  CommandPtr parse_coproc() {
    int coproc_line = cur().line;  // $LINENO / error line of the coproc command
    advance();  // coproc
    auto n = std::make_unique<CoprocCommand>();
    n->line = coproc_line;
    // Any word followed by a compound command is taken as the coproc NAME
    // (bash's grammar: COPROC WORD shell_command); an invalid name is
    // reported at execution, not as a syntax error (`coproc @ { :; }').
    bool name_then_cmd =
        cur().type == Tok::Word && !cur().quoted &&
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
    // The body may be on a later line (`foo()' then `{...}' next line): at EOF
    // the input is incomplete, not erroneous -- let the line reader fetch more
    // rather than hard-failing (which would leak the body to the top level).
    if (is(Tok::Eof)) incomplete = true;
    if (!err && !at_function_body()) {
      fail(is(Tok::Eof) ? std::string("unexpected end of file")
                        : std::string("near unexpected token `") + tok_to_text(cur()) + "'");
      return n;
    }
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
    // The body may be on a later line (`foo()' then `{...}' next line): at EOF
    // the input is incomplete, not erroneous -- let the line reader fetch more
    // rather than hard-failing (which would leak the body to the top level).
    if (is(Tok::Eof)) incomplete = true;
    if (!err && !at_function_body()) {
      fail(is(Tok::Eof) ? std::string("unexpected end of file")
                        : std::string("near unexpected token `") + tok_to_text(cur()) + "'");
      return n;
    }
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
      // An unterminated span may still have a here-document pending inside
      // it; the reader needs the delimiter's quoting to decide whether a
      // trailing `\' is a line continuation (comsub4.sub).
      if (toks.back().heredoc_eof) {
        res.heredoc_eof = true;
        res.heredoc_eof_delim = toks.back().heredoc_eof_delim;
        res.heredoc_eof_line = toks.back().heredoc_eof_line;
        res.heredoc_eof_quoted = toks.back().heredoc_eof_quoted;
      }
      return res;
    }
    newline_list();
    if (is(Tok::Eof)) return res;
    res.command = parse_list({});
    // Where the parse ended: bash leaves `line_number' on the last line it
    // consumed, and that is what a compound command which installs no line of
    // its own reports.  Take it before newline_list() eats the trailing
    // newlines, so a command ending at `done' names the `done' line.
    res.end_line = i > 0 ? toks[i - 1].line : 0;
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
      // A here-document delimited by end of input still warns even when the
      // surrounding construct failed to parse (`(cat <<EOF' at end of file).
      if (!toks.empty() && toks.back().heredoc_eof) {
        res.heredoc_eof = true;
        res.heredoc_eof_delim = toks.back().heredoc_eof_delim;
        res.heredoc_eof_line = toks.back().heredoc_eof_line;
        res.heredoc_eof_quoted = toks.back().heredoc_eof_quoted;
      }
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
    if (!toks.empty() && toks.back().comsub_unterm) {
      res.comsub_unterm = toks.back().comsub_unterm;
      res.comsub_unterm_line = toks.back().comsub_unterm_line;
    }
    return res;
  }
};

}  // namespace

// A redirection operator (its target word, and any leading fd number, form part
// of a command's prefix and do not end command position).
static bool is_redir_op(Tok t) {
  switch (t) {
    case Tok::Less: case Tok::Great: case Tok::DLess: case Tok::DGreat:
    case Tok::DLessDash: case Tok::TLess: case Tok::LessAnd: case Tok::GreatAnd:
    case Tok::LessGreat: case Tok::Clobber: case Tok::AndGreat: case Tok::AndDGreat:
      return true;
    default:
      return false;
  }
}

// An assignment-prefix word: NAME=, NAME+=, or NAME[subscript]= (NAME an
// identifier).  Such words precede the command word and do not end command
// position, so `VAR=val alias-name ...' still expands the alias.
static bool is_assignment_word(const std::string &w) {
  if (w.empty() || !(std::isalpha(static_cast<unsigned char>(w[0])) || w[0] == '_'))
    return false;
  size_t i = 1;
  while (i < w.size() && (std::isalnum(static_cast<unsigned char>(w[i])) || w[i] == '_'))
    i++;
  if (i < w.size() && w[i] == '[') {  // NAME[subscript]=
    int depth = 1;
    for (i++; i < w.size() && depth; i++) {
      if (w[i] == '[') depth++;
      else if (w[i] == ']') depth--;
    }
  }
  if (i < w.size() && w[i] == '+') i++;  // NAME+=
  return i < w.size() && w[i] == '=';
}

// Alias expansion is a TEXTUAL substitution performed on the input before it
// is parsed, mirroring bash's lexer-level push_string mechanics (parse.y
// alias_expand_token / shell_getc END_ALIAS): the body is spliced into the
// character stream in place of the alias word, so an unbalanced quote in a
// body continues into the following text, a trailing backslash escapes the
// next input character, and a `#' comments out the rest of the line.

// bash returns one virtual space when the expansion is exhausted so the
// following character cannot glue onto the body's last word (`alias
// foo='echo 0'; foo>&2' must not become an fd-0 redirect) -- EXCEPT when the
// body ends in a blank/newline/metacharacter, in an unquoted backslash, or
// inside a quoted string or comment (parse.y shell_getc, END_ALIAS return).
static bool alias_wants_sentinel(const std::string &body) {
  if (body.empty()) return false;
  char last = body.back();
  static const std::string meta = "()<>;&| \t\n";
  if (meta.find(last) != std::string::npos) return false;
  bool ub = false;      // trailing run of backslashes has odd parity
  char q = 0;           // open quote at end of body: ' or "
  bool comment = false; // body ends inside a `#' comment
  bool esc = false;     // next char is escaped (quote-state scan)
  char prev = 0;
  for (char c : body) {
    ub = !ub && c == '\\';  // bash's unquoted_backslash: a bare parity toggle
    if (comment) { if (c == '\n') comment = false; prev = c; continue; }
    if (esc) { esc = false; prev = c; continue; }
    if (q == '\'') { if (c == '\'') q = 0; prev = c; continue; }
    if (q == '"') {
      if (c == '\\') esc = true;
      else if (c == '"') q = 0;
      prev = c; continue;
    }
    if (c == '\\') { esc = true; prev = c; continue; }
    if (c == '\'' || c == '"') { q = c; prev = c; continue; }
    if (c == '#' && (prev == 0 || prev == ' ' || prev == '\t' || prev == '\n' ||
                     meta.find(prev) != std::string::npos))
      comment = true;
    prev = c;
  }
  return !ub && q == 0 && !comment;
}

// The result of textual alias expansion: the expanded input plus a per-byte
// map of the line each byte should REPORT.  Bytes spliced in from an alias
// body all report the line of the invoking word (bash does not advance
// $LINENO across an alias body); original bytes keep their physical line.
struct AliasExpansion {
  std::string text;
  std::vector<int> linemap;
  bool changed = false;
};

// One splice region: while a token starts inside the region, the alias NAME
// that produced it stays "being expanded" there and cannot expand again
// (bash's AL_BEINGEXPANDED).  A region whose alias value ended in a blank
// makes the next word after it eligible for expansion (bash's PST_ALEXPNEXT
// set on pop_string).
struct AliasZone {
  std::size_t start, end;
  std::string name;
  bool ends_blank;
};

static AliasExpansion alias_splice_text(const std::string &input,
                                        const std::map<std::string, std::string> &aliases,
                                        const std::map<std::string, std::string> &global_aliases,
                                        const std::map<std::string, std::string> &suffix_aliases,
                                        bool posix_mode) {
  AliasExpansion ax;
  ax.text = input;
  ax.linemap.resize(input.size());
  {
    int ln = 1;
    for (std::size_t b = 0; b < input.size(); b++) {
      ax.linemap[b] = ln;
      if (input[b] == '\n') ln++;
    }
  }
  if (aliases.empty() && global_aliases.empty() && suffix_aliases.empty()) return ax;

  static const std::set<std::string> kw = {
      "if", "then", "else", "elif", "do", "done", "{", "}", "while", "until",
      "for", "case", "select", "fi", "esac", "!", "time", "function"};

  std::vector<AliasZone> zones;

  // A word directly after a region whose alias value ended in a blank (only
  // blanks between) is eligible for expansion even outside command position.
  auto follows_blank_zone = [&](const Token &t) {
    for (const AliasZone &z : zones) {
      if (!z.ends_blank || z.end > t.start) continue;
      bool blanks = true;
      for (std::size_t b = z.end; b < t.start && blanks; b++)
        blanks = ax.text[b] == ' ' || ax.text[b] == '\t';
      if (blanks) return true;
    }
    return false;
  };
  auto zone_blocked = [&](const Token &t) {
    for (const AliasZone &z : zones)
      if (z.name == t.text && t.start >= z.start && t.start < z.end) return true;
    return false;
  };
  // Replace [start,end) of the working text with an alias NAME's replacement
  // string REP, keeping the line map and existing zones consistent.
  auto splice = [&](std::size_t start, std::size_t end, const std::string &rep,
                    const std::string &name, bool ends_blank) {
    long delta = static_cast<long>(rep.size()) - static_cast<long>(end - start);
    int inv_line = start < ax.linemap.size() ? ax.linemap[start]
                   : (ax.linemap.empty() ? 1 : ax.linemap.back());
    ax.text = ax.text.substr(0, start) + rep + ax.text.substr(end);
    ax.linemap.erase(ax.linemap.begin() + static_cast<long>(start),
                     ax.linemap.begin() + static_cast<long>(end));
    ax.linemap.insert(ax.linemap.begin() + static_cast<long>(start), rep.size(), inv_line);
    for (AliasZone &z : zones) {
      if (z.end <= start) continue;
      if (z.start >= end) { z.start += delta; z.end += delta; continue; }
      z.end += delta;  // the zone encloses the replaced span
    }
    zones.push_back({start, start + rep.size(), name, ends_blank});
    ax.changed = true;
  };

  bool progress = true;
  int guard = 0;
  while (progress && guard++ < 1000) {
    progress = false;
    std::vector<Token> toks = tokenize(ax.text);

    bool cmd_pos = true;
    // After `for'/`select' the next word is a variable name, not a command
    // word, so it must NOT be alias-expanded (`for j in ...' with `alias
    // j=...' keeps `j' literal, as bash does).
    bool name_next = false;
    // Nested `case' contexts: 0 = expecting the subject word, 1 = expecting
    // `in', 2 = in pattern position (no expansion -- patterns are not command
    // words), 3 = in a clause body.
    std::vector<int> case_state;

    for (std::size_t i = 0; i < toks.size() && !progress; i++) {
      const Token &t = toks[i];
      Tok ty = t.type;
      if (ty == Tok::SemiSemi || ty == Tok::SemiAnd || ty == Tok::SemiSemiAnd) {
        if (!case_state.empty()) case_state.back() = 2;
        cmd_pos = true; name_next = false;
        continue;
      }
      if (ty == Tok::Rparen) {
        if (!case_state.empty() && case_state.back() == 2) {
          case_state.back() = 3;
          cmd_pos = true; name_next = false;
        }
        continue;
      }
      if (ty == Tok::Newline || ty == Tok::Semi || ty == Tok::Amp || ty == Tok::Pipe ||
          ty == Tok::AndAnd || ty == Tok::OrOr || ty == Tok::Lparen || ty == Tok::PipeAnd) {
        // Inside case pattern position a newline does not start a command.
        if (case_state.empty() || case_state.back() != 2) { cmd_pos = true; name_next = false; }
        continue;
      }
      if (ty == Tok::IoNumber) continue;
      if (is_redir_op(ty)) {
        // The operator's target word: not a command word (skip it below), but
        // still eligible when it directly follows a blank-ending expansion
        // (bash checks PST_ALEXPNEXT for every token read) -- `alias c='< ';
        // ... c file' expands a `file' alias.  A leading redirection does not
        // end command position.
        if (i + 1 < toks.size() && toks[i + 1].type == Tok::Word) {
          const Token &w = toks[i + 1];
          if (!w.quoted && follows_blank_zone(w) && aliases.count(w.text) && !zone_blocked(w)) {
            const std::string &val = aliases.at(w.text);
            bool ends_blank = !val.empty() && (val.back() == ' ' || val.back() == '\t');
            splice(w.start, w.end, val + (alias_wants_sentinel(val) ? " " : ""), w.text, ends_blank);
            progress = true;
            continue;
          }
          i++;  // skip the target word
        }
        continue;  // cmd_pos unchanged
      }
      if (ty != Tok::Word) continue;

      // case machinery: subject and patterns are never expanded.
      if (!case_state.empty() && case_state.back() == 0) { case_state.back() = 1; continue; }
      if (!case_state.empty() && case_state.back() == 1) {
        if (!t.quoted && t.text == "in") case_state.back() = 2;
        continue;
      }
      if (!case_state.empty() && case_state.back() == 2) {
        if (!t.quoted && t.text == "esac") { case_state.pop_back(); cmd_pos = false; }
        continue;
      }

      if (name_next) { name_next = false; cmd_pos = false; continue; }

      bool eligible = !t.quoted && (cmd_pos || follows_blank_zone(t));

      // Posix.2 does not allow reserved words to be aliased: recognize them
      // before attempting alias expansion.  In default mode the alias check
      // comes first, so an eligible aliased reserved word expands (bash
      // parse.y read_token order).
      bool is_kw = !t.quoted && kw.count(t.text) && cmd_pos;
      if (!(posix_mode && is_kw) && eligible && aliases.count(t.text) && !zone_blocked(t)) {
        const std::string &val = aliases.at(t.text);
        bool ends_blank = !val.empty() && (val.back() == ' ' || val.back() == '\t');
        splice(t.start, t.end, val + (alias_wants_sentinel(val) ? " " : ""), t.text, ends_blank);
        progress = true;
        continue;
      }
      // zsh global aliases (`alias -g') expand in ANY word position.
      if (!t.quoted && global_aliases.count(t.text) && !zone_blocked(t)) {
        const std::string &val = global_aliases.at(t.text);
        splice(t.start, t.end, val, t.text, false);
        progress = true;
        continue;
      }
      // zsh suffix aliases (`alias -s ext=cmd'): a bare `file.ext' in command
      // position, whose `ext' has a suffix alias and which is not itself an
      // alias, runs `cmd file.ext' (the inserted command word takes over
      // command position, so this does not re-fire).
      if (eligible && !suffix_aliases.empty() && !aliases.count(t.text)) {
        std::size_t dot = t.text.rfind('.');
        if (dot != std::string::npos && dot + 1 < t.text.size()) {
          auto sit = suffix_aliases.find(t.text.substr(dot + 1));
          if (sit != suffix_aliases.end()) {
            splice(t.start, t.start, sit->second + " ", t.text, false);
            progress = true;
            continue;
          }
        }
      }

      if (is_kw) {
        cmd_pos = true;
        name_next = (t.text == "for" || t.text == "select");
        if (t.text == "case") case_state.push_back(0);
        else if (t.text == "esac" && !case_state.empty()) { case_state.pop_back(); cmd_pos = false; }
        continue;
      }
      // A prefix assignment keeps command position for the following word.
      if (cmd_pos && !t.quoted && is_assignment_word(t.text)) continue;
      cmd_pos = false;
    }
  }
  return ax;
}

// bash parses command-substitution content recursively at PARSE time, so a
// construct left open inside `$( )' is a syntax error of the OUTER parse:
// `echo $( if x; then echo foo )' reports "near unexpected token `)'", and a
// definite inner error appends "while looking for matching `)'" (`: $(case x
// in x) ;; x) done esac)' -> "near unexpected token `done' while ...").
// Spans starting `$((' are skipped (arithmetic or `$( (subshell' -- both
// validated elsewhere); quoted (') and escaped `$(' are literal.
ParseResult parse_with_aliases(const std::string &input,
                               const std::map<std::string, std::string> &aliases,
                               const std::map<std::string, std::string> &global_aliases,
                               const std::map<std::string, std::string> &suffix_aliases,
                               bool posix_mode, const std::vector<int> *cont_lines);

struct AliasTables {
  const std::map<std::string, std::string> *aliases;
  const std::map<std::string, std::string> *global_aliases;
  const std::map<std::string, std::string> *suffix_aliases;
};

static bool validate_comsubs(const std::vector<Token> &toks, bool posix_mode,
                             ParseResult &res, int depth,
                             const AliasTables *at = nullptr) {
  if (depth > 16) return true;
  for (const Token &t : toks) {
    if (t.type != Tok::Word) continue;
    const std::string &w = t.text;
    bool sq = false;
    for (size_t i = 0; i + 1 < w.size(); i++) {
      char c = w[i];
      if (sq) { if (c == '\'') sq = false; continue; }
      if (c == '\'') { sq = true; continue; }
      if (c == '\\') { i++; continue; }
      if (c != '$' || w[i + 1] != '(') continue;
      if (i + 2 < w.size() && w[i + 2] == '(') { i++; continue; }  // $(( form
      // The lexer's own scanner finds the closer (case patterns, quotes,
      // comments, and here-documents included).
      std::size_t jend = at ? comsub_span_end_aliased(w, i + 1, *at->aliases)
                            : comsub_span_end(w, i + 1);
      if (jend == std::string::npos) break;  // unterminated: already flagged
      size_t j = jend - 1;  // the `)'
      std::string inner = w.substr(i + 2, j - (i + 2));
      if (inner.find_first_not_of(" \t\n") == std::string::npos) { i = j; continue; }
      // bash parses substitution content with ALIASES ACTIVE (posix mode
      // requires it), so `$( switch foo in foo) ... esac )' with
      // `alias switch=case' is valid (comsub5.sub).
      ParseResult ir = at ? parse_with_aliases(inner, *at->aliases, *at->global_aliases,
                                               *at->suffix_aliases, posix_mode)
                          : parse(inner, posix_mode);
      if (!ir.ok) {
        res.ok = false;
        res.command.reset();
        res.error_line = t.line;
        if (ir.incomplete) {
          // The construct ran into the closing paren: bash blames the `)'.
          res.error = "near unexpected token `)'";
        } else if (ir.error.compare(0, 22, "near unexpected token ") == 0) {
          res.error = ir.error + " while looking for matching `)'";
        } else {
          res.error = ir.error;
        }
        return false;
      }
      i = j;
    }
  }
  return true;
}

ParseResult parse(const std::string &input, bool posix_mode,
                  const std::vector<int> *cont_lines) {
  Parser p(tokenize(input, posix_mode, nullptr, cont_lines));
  // Substitution content validates FIRST, as bash's recursive parser would:
  // its diagnosis wins over any knock-on error in the outer grammar.
  ParseResult pre;
  if (!validate_comsubs(p.toks, posix_mode, pre, 0)) return pre;
  return p.run();
}

ParseResult parse_with_aliases(const std::string &input,
                               const std::map<std::string, std::string> &aliases,
                               const std::map<std::string, std::string> &global_aliases,
                               const std::map<std::string, std::string> &suffix_aliases,
                               bool posix_mode, const std::vector<int> *cont_lines) {
  AliasExpansion ax =
      alias_splice_text(input, aliases, global_aliases, suffix_aliases, posix_mode);
  // The substitution scanner resolves `case'/`esac' through one alias level,
  // so a `$( switch x in y) ...;; esac )' span ends at the right `)'.
  std::vector<Token> toks = tokenize(ax.text, posix_mode, &aliases, cont_lines);
  if (ax.changed) {
    // Tokens report the line of the byte they start at: original bytes keep
    // their physical line, spliced bytes the invoking word's line.  The Eof
    // token falls on the line after the last content (bash parses input with
    // a trailing newline).
    for (Token &t : toks) {
      if (t.start < ax.linemap.size()) t.line = ax.linemap[t.start];
      else if (!ax.linemap.empty()) t.line = ax.linemap.back() + 1;
    }
  }
  Parser p(std::move(toks));
  AliasTables at{&aliases, &global_aliases, &suffix_aliases};
  ParseResult pre;
  if (!validate_comsubs(p.toks, posix_mode, pre, 0, &at)) return pre;
  return p.run();
}

}  // namespace gnash::core
