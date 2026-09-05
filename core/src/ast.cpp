// Copyright (c) 2026 Brian J. Fox
// Licensed under GPLv2 with the GPLv2-AI Exception.

// ast.cpp -- canonical rendering of the command AST (analogue of print_cmd.c).
//
// The rendering is a normalized, re-parseable form used for round-trip testing;
// it is not required to be byte-identical to the input, only stable and
// semantically faithful.

#include "gnash/core/ast.hpp"
#include "gnash/core/parser.hpp"
#include "gnash/core/quote.hpp"

namespace gnash::core {

namespace {

const char *op_string(RedirOp op) {
  switch (op) {
    case RedirOp::InputRedir: return "<";
    case RedirOp::OutputRedir: return ">";
    case RedirOp::AppendOutput: return ">>";
    case RedirOp::Clobber: return ">|";
    case RedirOp::InputOutput: return "<>";
    case RedirOp::DupInput: return "<&";
    case RedirOp::DupOutput: return ">&";
    case RedirOp::HereString: return "<<<";
    case RedirOp::HereDoc: return "<<";
    case RedirOp::HereDocStrip: return "<<-";
    case RedirOp::AndOutput: return "&>";
    case RedirOp::AndAppend: return "&>>";
  }
  return "?";
}

void invert_prefix(const Command *c, std::string &out) {
  if (c->flags & CMD_INVERT_RETURN) out += "! ";
  if (c->flags & CMD_TIME) out += (c->flags & CMD_TIME_POSIX) ? "time -p " : "time ";
}

}  // namespace

// The source fd bash prints for a redirection.  An explicit fd is kept; for the
// dup operators (`>&' / `<&') with no explicit fd bash shows the default it acts
// on (1 for output, 0 for input), so `>&2' displays as `1>&2'.  Every other
// redirection with an implicit fd prints none (returns -1).
int effective_source_fd(const Redirect &r) {
  if (r.source_fd >= 0) return r.source_fd;
  if (r.op == RedirOp::DupOutput) return 1;
  if (r.op == RedirOp::DupInput) return 0;
  return -1;
}

void print_redirects(const std::vector<Redirect> &redirs, std::string &out) {
  for (const Redirect &r : redirs) {
    out += ' ';
    if (!r.fd_var.empty()) { out += '{'; out += r.fd_var; out += '}'; }
    else { int sfd = effective_source_fd(r); if (sfd >= 0) out += std::to_string(sfd); }
    // bash separates the operator from its target with a space (`> bar', `< f'),
    // except the here-document and dup operators, whose target abuts (`<<EOF',
    // `1>&2').  (Matches MPrinter::print_redir, used for function bodies.)
    switch (r.op) {
      case RedirOp::HereDoc: out += "<<"; out += r.target.text; continue;
      case RedirOp::HereDocStrip: out += "<<-"; out += r.target.text; continue;
      // A close (`{v}<&-') is one operation regardless of direction; bash's
      // printer renders it `>&-'.
      case RedirOp::DupInput:
        out += (!r.fd_var.empty() && r.target.text == "-") ? ">&" : "<&";
        out += r.target.text;
        continue;
      case RedirOp::DupOutput: out += ">&"; out += r.target.text; continue;
      default: break;
    }
    out += op_string(r.op);
    out += ' ';
    out += r.target.text;
  }
}

std::string to_string(const Command *c) {
  std::string s;
  if (c) c->print(s);
  return s;
}

void SimpleCommand::print(std::string &out) const {
  invert_prefix(this, out);
  for (size_t i = 0; i < words.size(); i++) {
    if (i) out += ' ';
    out += words[i].text;
  }
  print_redirects(redirects, out);
}

void Connection::print(std::string &out) const {
  invert_prefix(this, out);
  if (first) first->print(out);
  switch (conn) {
    case Connector::And: out += " && "; break;
    case Connector::Or: out += " || "; break;
    case Connector::Pipe: out += " | "; break;
    case Connector::Amp: out += " &"; break;
    case Connector::Semi:
    case Connector::Newline: out += "; "; break;
  }
  if (second) {
    if (conn == Connector::Amp) out += ' ';
    second->print(out);
  }
}

void Subshell::print(std::string &out) const {
  invert_prefix(this, out);
  // bash renders a subshell with inner spaces: `( list )'.
  out += "( ";
  if (body) body->print(out);
  out += " )";
  print_redirects(redirects, out);
}

void Group::print(std::string &out) const {
  invert_prefix(this, out);
  out += "{ ";
  if (body) body->print(out);
  out += "; }";
  print_redirects(redirects, out);
}

void IfCommand::print(std::string &out) const {
  invert_prefix(this, out);
  out += "if ";
  if (cond) cond->print(out);
  out += "; then ";
  if (then_part) then_part->print(out);
  if (else_part) {
    out += "; else ";
    else_part->print(out);
  }
  out += "; fi";
  print_redirects(redirects, out);
}

void LoopCommand::print(std::string &out) const {
  invert_prefix(this, out);
  out += until ? "until " : "while ";
  if (cond) cond->print(out);
  out += "; do ";
  if (body) body->print(out);
  out += "; done";
  print_redirects(redirects, out);
}

void ForCommand::print(std::string &out) const {
  invert_prefix(this, out);
  if (is_arith) {
    // bash stores an empty arith-for section as the constant `1', so its
    // reconstructed form (declare -f, xtrace) shows `for ((1; 1; 1))'.
    out += "for ((";
    out += a_init.empty() ? "1" : a_init;
    out += "; ";
    out += a_cond.empty() ? "1" : a_cond;
    out += "; ";
    out += a_update.empty() ? "1" : a_update;
    out += "))";
  } else {
    out += is_select ? "select " : "for ";
    out += var;
    if (words_present) {
      out += " in";
      for (const Word &w : words) {
        out += ' ';
        out += w.text;
      }
    }
  }
  out += "; do ";
  if (body) body->print(out);
  out += "; done";
  print_redirects(redirects, out);
}

void CondCommand::print(std::string &out) const {
  invert_prefix(this, out);
  out += "[[ ";
  out += expression;
  out += " ]]";
  print_redirects(redirects, out);
}

void ArithCommand::print(std::string &out) const {
  invert_prefix(this, out);
  // `expression' keeps its leading/trailing blanks (so errors and xtrace
  // reproduce bash's raw text); the printer wraps the raw text directly,
  // exactly as bash does (`((  x + 1  ))' stays double-spaced, `((x+1))'
  // stays unspaced).
  out += "((";
  out += expression;
  out += "))";
  print_redirects(redirects, out);
}

void CoprocCommand::print(std::string &out) const {
  out += "coproc ";
  if (!name.empty()) {
    out += name;
    out += ' ';
  }
  if (body) body->print(out);
  print_redirects(redirects, out);
}

void CaseCommand::print(std::string &out) const {
  invert_prefix(this, out);
  out += "case ";
  out += word.text;
  out += " in ";
  for (const CaseClause &cl : clauses) {
    for (size_t i = 0; i < cl.patterns.size(); i++) {
      if (i) out += " | ";
      out += cl.patterns[i].text;
    }
    out += ") ";
    if (cl.body) cl.body->print(out);
    out += " ;; ";
  }
  out += "esac";
  print_redirects(redirects, out);
}

void FunctionDef::print(std::string &out) const {
  out += name;
  out += " () ";
  if (body) body->print(out);
  print_redirects(redirects, out);
}


// ---- bash-format multi-line printing (print_cmd.c's named_function_string) --
//
// `type', `declare -f', and `set' display function bodies in bash's canonical
// indented form; these must match bash byte-for-byte (including trailing
// blanks after `NAME () ', `{ ', and `case W in ').

namespace {

std::string canonical_word(const std::string &w);

struct MPrinter {
  std::string out;
  bool pretty = false;  // --pretty-print (top level): `;' lists join inline
  bool comsub = false;  // print_comsub: preserve `;' (inline) vs newline connectors
  std::vector<const Redirect *> pending_heredocs;
  bool last_was_heredoc = false;   // a simple command ended with a here-document
  bool compound_heredoc = false;   // a for/while/if body ended with a here-document
  bool last_was_amp = false;

  void ind(int n) { out.append(static_cast<size_t>(n), ' '); }
  void nl(int n) { out += '\n'; ind(n); }

  void print_redir(const Redirect &r) {
    out += ' ';
    if (!r.fd_var.empty()) { out += '{'; out += r.fd_var; out += '}'; }
    else { int sfd = effective_source_fd(r); if (sfd >= 0) out += std::to_string(sfd); }
    switch (r.op) {
      case RedirOp::HereDoc: out += "<<"; out += r.target.text; return;
      case RedirOp::HereDocStrip: out += "<<-"; out += r.target.text; return;
      // A close (`{v}<&-') prints as `>&-', as in print_redirects above.
      case RedirOp::DupInput:
        out += (!r.fd_var.empty() && r.target.text == "-") ? ">&" : "<&";
        out += r.target.text;
        return;
      case RedirOp::DupOutput: out += ">&"; out += r.target.text; return;
      default: break;
    }
    out += op_string(r.op);
    out += ' ';
    out += r.target.text;
  }

  void print_redirs(const std::vector<Redirect> &rs) {
    for (const Redirect &r : rs) {
      print_redir(r);
      if (r.op == RedirOp::HereDoc || r.op == RedirOp::HereDocStrip)
        pending_heredocs.push_back(&r);
    }
  }

  // Emit queued here-document bodies right after the command's line.
  bool flush_heredocs() {
    if (pending_heredocs.empty()) return false;
    for (const Redirect *r : pending_heredocs) {
      out += '\n';
      out += r->heredoc_body;  // body lines keep their own newlines
      out += r->target.text;
    }
    pending_heredocs.clear();
    return true;
  }

  // One statement (a sequence element) at INDENT; records how it ended so the
  // caller knows whether to append `;'.
  void stmt(const Command *c, int I) {
    last_was_amp = false;
    last_was_heredoc = false;
    compound_heredoc = false;
    inline_cmd(c, I);
    // A for/while/if whose body ended with a here-document leaves last_was_heredoc
    // set (its body flushed inside).  bash then suppresses the `;' separator but,
    // unlike a direct simple-command here-document, adds NO blank line before the
    // next statement -- so track it separately.  A simple command instead leaves
    // its here-documents pending; flushing them marks the direct (blank-line) case.
    bool body_heredoc = last_was_heredoc;
    last_was_heredoc = compound_heredoc = false;
    if (body_heredoc) compound_heredoc = true;
    else if (flush_heredocs()) last_was_heredoc = true;
  }

  // Statement sequences: each Semi-connected element on its own line.  The
  // parser builds these left-associated, so recurse into both sides.
  void list(const Command *c, int I) {
    const auto *cn = dynamic_cast<const Connection *>(c);
    if (cn && (cn->conn == Connector::Semi || cn->conn == Connector::Newline)) {
      list(cn->first.get(), I);
      if (cn->second) {
        if (last_was_heredoc) { out += '\n'; nl(I); }  // simple heredoc: blank line
        else if (compound_heredoc) { nl(I); }          // compound heredoc: no `;', no blank
        else if (last_was_amp) { nl(I); }
        // print_comsub preserves the original separator: a `;' stays on the
        // same line (`; '), a newline advances (`\n').  This is what makes a
        // funsub body's executed line numbers match bash (comsub2.tests).
        else if (comsub) {
          if (cn->conn == Connector::Newline) { out += '\n'; ind(I); }
          else { out += "; "; }
        }
        // Outside a function definition bash joins a `;'/newline list inline
        // (`done <<< a; echo done;'); inside one, each command starts its own
        // line (print_cmd.c: the_printed_command's inside_function_def test).
        else if (pretty) { out += "; "; }
        else { out += ';'; nl(I); }
        list(cn->second.get(), I);
      }
      return;
    }
    if (cn && cn->conn == Connector::Amp) {
      list(cn->first.get(), I);
      if (!last_was_amp) out += " &";
      last_was_amp = true;
      last_was_heredoc = false;
      if (cn->second) {
        out += ' ';  // `a & b' stays on one line
        last_was_amp = false;
        list(cn->second.get(), I);
      }
      return;
    }
    stmt(c, I);
  }

  // Terminate a clause body (then/else/do): append `;' unless the last
  // statement ended with `&' or a here-document.
  void clause_semi() {
    if (!last_was_amp && !last_was_heredoc && !compound_heredoc) out += ';';
  }

  // Move to a compound closer's line (`done'/`fi'/`}'/`esac').  When the body
  // ended with a here-document, bash's deferred-heredoc flush leaves a trailing
  // newline, so the closer follows a blank line.
  void close_nl(int I) {
    if (last_was_heredoc) out += '\n';
    nl(I);
  }

  // Render one command; compounds span lines from INDENT.
  void inline_cmd(const Command *c, int I) {
    if (c == nullptr) return;
    if (c->flags & CMD_INVERT_RETURN) out += "! ";
    if (c->flags & CMD_TIME) out += (c->flags & CMD_TIME_POSIX) ? "time -p " : "time ";

    if (const auto *sc = dynamic_cast<const SimpleCommand *>(c)) {
      for (size_t i = 0; i < sc->words.size(); i++) {
        if (i) out += ' ';
        out += canonical_word(sc->words[i].text);
      }
      print_redirs(sc->redirects);
      return;
    }
    if (const auto *cn = dynamic_cast<const Connection *>(c)) {
      switch (cn->conn) {
        case Connector::And:
          inline_cmd(cn->first.get(), I);
          out += " && ";
          inline_cmd(cn->second.get(), I);
          return;
        case Connector::Or:
          inline_cmd(cn->first.get(), I);
          out += " || ";
          inline_cmd(cn->second.get(), I);
          return;
        case Connector::Pipe:
          inline_cmd(cn->first.get(), I);
          out += " | ";
          inline_cmd(cn->second.get(), I);
          return;
        case Connector::Amp:
          inline_cmd(cn->first.get(), I);
          out += " &";
          last_was_amp = true;
          if (cn->second) {
            out += ' ';
            inline_cmd(cn->second.get(), I);
            last_was_amp = false;
          }
          return;
        case Connector::Semi:
        case Connector::Newline:
          inline_cmd(cn->first.get(), I);
          out += "; ";
          inline_cmd(cn->second.get(), I);
          return;
      }
      return;
    }
    if (const auto *g = dynamic_cast<const Group *>(c)) {
      out += "{ ";
      nl(I + 4);
      list(g->body.get(), I + 4);
      close_nl(I);
      out += '}';
      // A brace group flushes its body's here-documents and is followed by the
      // normal `;' separator (unlike for/while/if), so clear the flag.
      last_was_heredoc = false;
      print_redirs(g->redirects);
      return;
    }
    if (const auto *ss = dynamic_cast<const Subshell *>(c)) {
      out += "( ";
      inline_cmd(ss->body.get(), I);
      // A here-document in the subshell body is flushed before the closing `)'
      // (bash's PRINT_DEFERRED_HEREDOCS), which then starts its own line.
      if (flush_heredocs()) out += "\n )";
      else out += " )";
      last_was_heredoc = false;
      print_redirs(ss->redirects);
      return;
    }
    if (const auto *ic = dynamic_cast<const IfCommand *>(c)) {
      out += "if ";
      if (inline_line_here(ic->cond.get())) { nl(I); out += "then"; }
      else out += "; then";
      nl(I + 4);
      list(ic->then_part.get(), I + 4);
      clause_semi();
      close_nl(I);
      if (ic->else_part) {
        out += "else";
        nl(I + 4);
        list(ic->else_part.get(), I + 4);
        clause_semi();
        close_nl(I);
      }
      out += "fi";
      print_redirs(ic->redirects);
      return;
    }
    if (const auto *lc = dynamic_cast<const LoopCommand *>(c)) {
      out += lc->until ? "until " : "while ";
      if (inline_line_here(lc->cond.get())) { nl(I); out += "do"; }
      else out += "; do";
      nl(I + 4);
      list(lc->body.get(), I + 4);
      clause_semi();
      close_nl(I);
      out += "done";
      print_redirs(lc->redirects);
      return;
    }
    if (const auto *fc = dynamic_cast<const ForCommand *>(c)) {
      if (fc->is_arith) {
        // An empty section is stored/printed as `1' (bash make_arith_for_expr).
        out += "for ((";
        out += fc->a_init.empty() ? "1" : fc->a_init;
        out += "; ";
        out += fc->a_cond.empty() ? "1" : fc->a_cond;
        out += "; ";
        out += fc->a_update.empty() ? "1" : fc->a_update;
        out += "))";
      } else {
        out += fc->is_select ? "select " : "for ";
        out += fc->var;
        out += " in ";
        if (fc->words_present) {
          for (size_t i = 0; i < fc->words.size(); i++) {
            if (i) out += ' ';
            out += canonical_word(fc->words[i].text);
          }
        } else {
          out += "\"$@\"";  // bash canonicalizes a bare `for i'
        }
        out += ';';
      }
      nl(I);
      out += "do";
      nl(I + 4);
      list(fc->body.get(), I + 4);
      clause_semi();
      close_nl(I);
      out += "done";
      print_redirs(fc->redirects);
      return;
    }
    if (const auto *cc = dynamic_cast<const CaseCommand *>(c)) {
      out += "case ";
      out += cc->word.text;
      out += " in ";
      for (const CaseClause &cl : cc->clauses) {
        nl(I + 4);
        for (size_t i = 0; i < cl.patterns.size(); i++) {
          if (i) out += " | ";
          out += cl.patterns[i].text;
        }
        out += ')';
        out += '\n';
        if (cl.body) {
          ind(I + 8);
          list(cl.body.get(), I + 8);
        }
        out += '\n';
        ind(I + 4);
        out += cl.terminator == 1 ? ";&" : cl.terminator == 2 ? ";;&" : ";;";
      }
      nl(I);
      out += "esac";
      print_redirs(cc->redirects);
      return;
    }
    if (const auto *fd = dynamic_cast<const FunctionDef *>(c)) {
      // A function definition nested in another function's body always prints
      // in the `function NAME ()' form, regardless of how it was written -- the
      // outer (top-level) display uses the bare `NAME ()' form instead.
      out += "function ";
      out += fd->name;
      out += " () ";
      out += '\n';
      ind(I);
      print_func_body(fd->body.get(), I);
      return;
    }
    if (const auto *cp = dynamic_cast<const CoprocCommand *>(c)) {
      out += "coproc ";
      // bash prints the coproc name only when the body is a compound command
      // (`{ ... }' / `( ... )'), defaulting to COPROC; a simple-command coproc
      // is printed without a name.  The body then renders like any command, so a
      // here-document in it is flushed at the right place.
      bool compound = dynamic_cast<const Group *>(cp->body.get()) ||
                      dynamic_cast<const Subshell *>(cp->body.get());
      if (compound) {
        out += cp->name.empty() ? "COPROC" : cp->name;
        out += ' ';
      }
      inline_cmd(cp->body.get(), I);
      print_redirs(cp->redirects);
      return;
    }
    // [[ ]] / (( )) / anything else: the single-line rendering.
    std::string one;
    c->print(one);
    out += one;
  }

  // A condition (if/while) or subshell body on one line.
  // Collect the here-document redirections of a command (in source order) so a
  // condition line can emit their bodies.  The condition is a simple command or
  // a pipeline/and-or of them, so recurse through Connection nodes.
  static void collect_heredocs(const Command *c, std::vector<const Redirect *> &hds) {
    if (c == nullptr) return;
    if (const auto *sc = dynamic_cast<const SimpleCommand *>(c)) {
      for (const Redirect &r : sc->redirects)
        if (r.op == RedirOp::HereDoc || r.op == RedirOp::HereDocStrip) hds.push_back(&r);
    } else if (const auto *cn = dynamic_cast<const Connection *>(c)) {
      collect_heredocs(cn->first.get(), hds);
      collect_heredocs(cn->second.get(), hds);
    }
  }

  // Print a compound-command header line (an if/while condition) inline, then
  // emit any here-document bodies it carries (bash prints them right after the
  // condition line, before `then'/`do').  Returns true if a body was emitted,
  // so the caller starts `then'/`do' on a fresh line instead of `; '.
  bool inline_line_here(const Command *c) {
    inline_line(c);
    std::vector<const Redirect *> hds;
    collect_heredocs(c, hds);
    for (const Redirect *r : hds) {
      out += '\n';
      out += r->heredoc_body;  // body lines keep their own newlines
      out += r->target.text;
    }
    return !hds.empty();
  }
  void inline_line(const Command *c) {
    std::string one;
    if (c) c->print(one);
    out += one;
  }

  // A function body always displays as a `{ ... }' group.  A brace-group body
  // supplies its own braces; any other compound command (subshell, if, for, ...)
  // is wrapped in an explicit group, matching bash's named_function_string.
  void print_func_body(const Command *body, int I) {
    if (dynamic_cast<const Group *>(body)) {
      inline_cmd(body, I);
      return;
    }
    out += "{ ";
    nl(I + 4);
    inline_cmd(body, I + 4);
    last_was_heredoc = flush_heredocs();
    close_nl(I);
    out += '}';
  }
};

// bash's lexer expands `$'...'' (ansicstr) and `$"..."' at read_token_word
// time, so a reconstructed word never shows the `$...' source: `v=$'\001''
// displays as `v='^A'' and `v=$"msg"' as `v="msg"'.  gnash keeps the source
// text in the AST, so the printer performs the same expansion.

void utf8_append(std::string &out, unsigned long cp) {
  if (cp < 0x80) out += static_cast<char>(cp);
  else if (cp < 0x800) {
    out += static_cast<char>(0xC0 | (cp >> 6));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    out += static_cast<char>(0xE0 | (cp >> 12));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else {
    out += static_cast<char>(0xF0 | (cp >> 18));
    out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  }
}

// Decode the escapes of an ANSI-C string body (the text between $' and '),
// bash's ansicstr: unknown escapes keep the backslash, a NUL truncates.
std::string ansi_c_decode(const std::string &s) {
  std::string out;
  size_t i = 0;
  auto hexval = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  while (i < s.size()) {
    if (s[i] != '\\') { out += s[i++]; continue; }
    if (i + 1 >= s.size()) { out += '\\'; break; }
    char e = s[i + 1];
    i += 2;
    char c = 0;
    switch (e) {
      case 'a': c = '\a'; break;
      case 'b': c = '\b'; break;
      case 'e': case 'E': c = '\033'; break;
      case 'f': c = '\f'; break;
      case 'n': c = '\n'; break;
      case 'r': c = '\r'; break;
      case 't': c = '\t'; break;
      case 'v': c = '\v'; break;
      case '\\': case '\'': case '"': case '?': c = e; break;
      case 'x': case 'u': case 'U': {
        int maxd = e == 'x' ? 2 : e == 'u' ? 4 : 8;
        unsigned long v = 0;
        int nd = 0;
        while (nd < maxd && i < s.size() && hexval(s[i]) >= 0) {
          v = v * 16 + hexval(s[i]);
          i++; nd++;
        }
        if (nd == 0) { out += '\\'; out += e; continue; }  // no digits: literal
        if (e == 'x') c = static_cast<char>(v);
        else { utf8_append(out, v); continue; }
        break;
      }
      case '0': case '1': case '2': case '3':
      case '4': case '5': case '6': case '7': {
        int v = e - '0', nd = 1;
        while (nd < 3 && i < s.size() && s[i] >= '0' && s[i] <= '7') {
          v = v * 8 + (s[i] - '0');
          i++; nd++;
        }
        c = static_cast<char>(v);
        break;
      }
      case 'c': {
        if (i >= s.size()) { out += "\\c"; continue; }
        char ch = s[i++];
        if (ch == '\\' && i < s.size() && s[i] == '\\') i++;  // \c\\ = ctrl-backslash
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)) & 0x1f);
        break;
      }
      default: out += '\\'; out += e; continue;
    }
    if (c == '\0') return out;  // NUL truncates the decoded string (C string)
    out += c;
  }
  return out;
}

// Expand $'...' / $"..." spans of a word into their lexed display form,
// tracking the quoting contexts bash's lexer distinguishes: not inside '...'
// or `...` or after $$; active at top level, inside "..."-embedded $(...),
// inside ${...} and inside $(...).
std::string ansi_expand_word(const std::string &w) {
  std::string out;
  size_t i = 0;
  while (i < w.size()) {
    char c = w[i];
    if (c == '\\') {  // escaped char: copy both
      out += c;
      if (i + 1 < w.size()) out += w[i + 1];
      i = std::min(i + 2, w.size());
      continue;
    }
    if (c == '\'') {  // single-quoted span: verbatim
      size_t j = w.find('\'', i + 1);
      if (j == std::string::npos) { out += w.substr(i); break; }
      out += w.substr(i, j - i + 1);
      i = j + 1;
      continue;
    }
    if (c == '`') {  // backquotes are re-lexed at expansion time: verbatim
      out += c;
      i++;
      while (i < w.size()) {
        if (w[i] == '\\' && i + 1 < w.size()) { out += w[i]; out += w[i + 1]; i += 2; continue; }
        out += w[i];
        if (w[i++] == '`') break;
      }
      continue;
    }
    if (c == '"') {  // double quotes: $'..' stays, but a nested $(..) re-lexes
      out += c;
      i++;
      while (i < w.size()) {
        if (w[i] == '\\' && i + 1 < w.size()) { out += w[i]; out += w[i + 1]; i += 2; continue; }
        if (w[i] == '"') { out += '"'; i++; break; }
        if (w[i] == '$' && i + 1 < w.size() && w[i + 1] == '(') {
          size_t j = i + 1;
          for (int depth = 0; j < w.size(); j++) {
            if (w[j] == '(') depth++;
            else if (w[j] == ')' && --depth == 0) break;
          }
          if (j >= w.size()) { out += w.substr(i); i = w.size(); break; }
          out += "$(";
          out += ansi_expand_word(w.substr(i + 2, j - (i + 2)));
          out += ')';
          i = j + 1;
          continue;
        }
        out += w[i++];
      }
      continue;
    }
    if (c == '$' && i + 1 < w.size()) {
      char n = w[i + 1];
      if (n == '$') { out += "$$"; i += 2; continue; }  // PID, not a quote opener
      if (n == '\'') {
        size_t j = i + 2;  // find the closing ', honoring backslash escapes
        while (j < w.size() && w[j] != '\'') j += (w[j] == '\\' && j + 1 < w.size()) ? 2 : 1;
        if (j >= w.size()) { out += w.substr(i); break; }
        out += single_quote(ansi_c_decode(w.substr(i + 2, j - (i + 2))));
        i = j + 1;
        continue;
      }
      if (n == '"') { i++; continue; }  // $"..." -> "..." (locale string)
      if (n == '(') {  // re-lexed context: expand inside
        size_t j = i + 1;
        for (int depth = 0; j < w.size(); j++) {
          if (w[j] == '(') depth++;
          else if (w[j] == ')' && --depth == 0) break;
        }
        if (j >= w.size()) { out += w.substr(i); break; }
        out += "$(";
        out += ansi_expand_word(w.substr(i + 2, j - (i + 2)));
        out += ')';
        i = j + 1;
        continue;
      }
    }
    out += c;
    i++;
  }
  return out;
}

// Re-render `$(...)' command substitutions in W through the parser, as bash
// prints the parsed form ("$( echo hi )" becomes "$(echo hi)").  On any parse
// trouble the original text is kept.  ANSI-C / locale strings are expanded
// first, as bash's lexer has already done that by the time it prints.
std::string canonical_word(const std::string &w_in) {
  std::string w = ansi_expand_word(w_in);
  size_t at = w.find("$(");
  if (at == std::string::npos) return w;
  std::string out;
  size_t i = 0;
  while (i < w.size()) {
    if (w[i] == '\'') {  // skip single-quoted spans
      size_t j = w.find('\'', i + 1);
      out += w.substr(i, j == std::string::npos ? std::string::npos : j - i + 1);
      if (j == std::string::npos) return w;
      i = j + 1;
      continue;
    }
    if (w[i] == '$' && i + 1 < w.size() && w[i + 1] == '(' &&
        !(i + 2 < w.size() && w[i + 2] == '(')) {
      int depth = 0;
      size_t j = i + 1;
      for (; j < w.size(); j++) {
        if (w[j] == '(') depth++;
        else if (w[j] == ')' && --depth == 0) break;
      }
      if (j >= w.size()) return w;
      std::string inner = w.substr(i + 2, j - (i + 2));
      ParseResult pr = parse(inner);
      if (pr.ok && pr.command) {
        out += "$(";
        std::string one;
        pr.command->print(one);
        // A command with no words but a leading redirection (`$(< file)') prints
        // with a leading space from print_redirects; bash abuts it to the `$('.
        size_t s = one.find_first_not_of(' ');
        out += (s == std::string::npos) ? one : one.substr(s);
        out += ')';
      } else {
        out += w.substr(i, j - i + 1);
      }
      i = j + 1;
      continue;
    }
    out += w[i++];
  }
  return out;
}

}  // namespace

// bash's assignment(): NAME[sub]=... where NAME is a valid identifier.
static bool func_name_is_assignment(const std::string &s) {
  size_t i = 0;
  if (s.empty() || (!std::isalpha((unsigned char)s[0]) && s[0] != '_')) return false;
  while (i < s.size() && (std::isalnum((unsigned char)s[i]) || s[i] == '_')) i++;
  if (i < s.size() && s[i] == '[') {
    size_t j = s.find(']', i);
    if (j == std::string::npos) return false;
    i = j + 1;
  }
  return i < s.size() && s[i] == '=';
}

// The bash-canonical display text of one word ($'...' decoded, command
// substitutions re-rendered) -- what xtrace's compound-assignment printer
// shows for each element.
std::string canonical_word_text(const std::string &w) { return canonical_word(w); }

// bash --pretty-print: each top-level command renders in the multiline
// (declare -f) format followed by a blank line; a top-level newline list
// prints as separate commands, exactly as bash's one-command-per-read loop.
std::string pretty_print_string(const Command *c) {
  if (c == nullptr) return std::string();
  if (const auto *cn = dynamic_cast<const Connection *>(c)) {
    if (cn->conn == Connector::Newline)
      return pretty_print_string(cn->first.get()) +
             pretty_print_string(cn->second.get());
  }
  MPrinter p;
  p.pretty = true;
  p.inline_cmd(c, 0);
  p.flush_heredocs();
  p.out += "\n\n";
  return p.out;
}

// bash's parse_comsub renders the parsed body with print_comsub and executes
// THAT reprinted text, so a funsub/command-substitution body's runtime line
// numbers follow the canonical rendering (each `;' inline, each newline a new
// line) rather than the physical source layout.  Reproducing it here lets a
// funsub's $LINENO and error lines match bash (comsub2.tests).
std::string comsub_reprint_string(const Command *c) {
  if (c == nullptr) return std::string();
  MPrinter p;
  p.comsub = true;
  p.list(c, 0);
  p.flush_heredocs();
  return p.out;
}

std::string named_function_string(const std::string &name, const Command *body,
                                  bool posix) {
  MPrinter p;
  // bash prefixes `function ' only to an assignment-shaped name (`a=2' would
  // reparse as an assignment without it); an all-digit or otherwise funky name
  // prints bare in BOTH modes (`11111 () ', even under posix).
  (void)posix;
  bool prefix = func_name_is_assignment(name);
  p.out = (prefix ? "function " : "") + name + " () ";
  p.out += '\n';
  p.print_func_body(body, 0);
  return p.out;
}

}  // namespace gnash::core
