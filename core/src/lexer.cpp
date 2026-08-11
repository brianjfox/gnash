// Copyright (c) 2026 Brian J. Fox
// Licensed under GPLv2 with the GPLv2-AI Exception.

// lexer.cpp -- shell tokenizer (see lexer.hpp).

#include "gnash/core/lexer.hpp"
#include "gnash/core/subscript.hpp"

#include <cctype>
#include <cstring>
#include <cstdlib>
#include <cwchar>
#include <set>

namespace gnash::core {

const char *tok_name(Tok t) {
  switch (t) {
    case Tok::Word: return "WORD";
    case Tok::IoNumber: return "IO_NUMBER";
    case Tok::Newline: return "NEWLINE";
    case Tok::Amp: return "&";
    case Tok::Semi: return ";";
    case Tok::Pipe: return "|";
    case Tok::AndAnd: return "&&";
    case Tok::OrOr: return "||";
    case Tok::SemiSemi: return ";;";
    case Tok::SemiAnd: return ";&";
    case Tok::SemiSemiAnd: return ";;&";
    case Tok::PipeAnd: return "|&";
    case Tok::Lparen: return "(";
    case Tok::Rparen: return ")";
    case Tok::Less: return "<";
    case Tok::Great: return ">";
    case Tok::DLess: return "<<";
    case Tok::DGreat: return ">>";
    case Tok::DLessDash: return "<<-";
    case Tok::TLess: return "<<<";
    case Tok::LessAnd: return "<&";
    case Tok::GreatAnd: return ">&";
    case Tok::LessGreat: return "<>";
    case Tok::Clobber: return ">|";
    case Tok::AndGreat: return "&>";
    case Tok::AndDGreat: return "&>>";
    case Tok::Eof: return "EOF";
  }
  return "?";
}

namespace {

struct Pending {
  std::size_t index;   // token index of the delimiter word
  std::string delim;   // dequoted delimiter
  bool strip;          // <<- strips leading tabs
  bool quoted;         // delimiter was quoted
  // A here-document left pending when a $(...) closed on the same line: the
  // collected body is SPLICED into the word's text at splice_at (before the
  // substitution's closer) so the inner command reads it, instead of being
  // attached to a redirection.
  bool comsub = false;
  std::size_t splice_at = 0;
};

struct Lexer {
  const std::string &in;
  std::size_t pos = 0;
  std::size_t n;
  std::vector<Token> out;
  std::vector<Pending> pending;
  // Alias table used ONLY to recognize `case'/`esac' through one alias level
  // while scanning a substitution span (see comsub_span_end_aliased).
  const std::map<std::string, std::string> *span_aliases = nullptr;
  // Heredocs pending when a $(...) closed mid-line (bash warns `command
  // substitution: N unterminated here-document'); converted into comsub
  // Pending entries when the containing word token lands.
  struct CarryHd { std::string delim; bool strip; std::size_t splice_at; };
  std::vector<CarryHd> comsub_carry;
  int comsub_unterm = 0;
  int comsub_unterm_line = 0;
  int awaiting = -1;  // -1 none, 0 <<, 1 <<-
  int cmd_heredocs = 0;  // here-documents registered for the current command
  bool heredoc_overflow = false;  // > HEREDOC_MAX on one command (fatal, bash)
  int heredoc_overflow_line = 0;
  bool unterminated = false;
  char unterm_close = 0;  // the closer we were looking for at EOF
  // The line the unterminated span STARTED on.  bash's parse_matched_pair
  // reports `start_lineno' for a quote, a backquote and `${', so an
  // unterminated one names where it opened rather than where input ran out.
  // Left 0 for `$(' -- bash's parse_comsub reports the end-of-input line
  // there, which is what the Eof token already carries.
  int unterm_line = 0;
  bool heredoc_eof = false;        // here-doc body delimited by end of input
  std::string heredoc_eof_delim;
  int heredoc_eof_line = 0;
  bool heredoc_eof_quoted = false;  // that here-doc's delimiter was quoted
  std::size_t line_scanned = 0;  // bytes already counted for line numbering
  int cur_line = 1;              // 1-based line at line_scanned
  // Lines at which a spliced-away line continuation sat; see lexer.hpp.
  const std::vector<int> *cont_lines = nullptr;

  // Line number of the byte at `start` (pos advances monotonically).
  int line_for(std::size_t start) {
    while (line_scanned < start && line_scanned < n) {
      if (in[line_scanned] == '\n') cur_line++;
      line_scanned++;
    }
    if (!cont_lines) return cur_line;
    // Every continuation that joined an EARLIER line pushed this one down by
    // one physical line; a token on the joining line itself keeps that line,
    // which is where bash reports the command as starting.
    int extra = 0;
    for (int c : *cont_lines) {
      if (c >= cur_line) break;
      extra++;
    }
    return cur_line + extra;
  }

  static bool is_assignment_prefix(const std::string &w) {
    // name= / name+= / name[..]= (the `(' that follows starts an array value)
    if (w.size() < 2 || w.back() != '=') return false;
    char c0 = w[0];
    return std::isalpha(static_cast<unsigned char>(c0)) || c0 == '_';
  }

  // The word so far is a plain name (`a', `foo_1'): a candidate array-assignment
  // target whose `[subscript]' should be scanned as one word.
  static bool is_name_word(const std::string &w) {
    if (w.empty() || !(std::isalpha(static_cast<unsigned char>(w[0])) || w[0] == '_'))
      return false;
    for (char c : w)
      if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_')) return false;
    return true;
  }

  // Offset of the `]' closing the array subscript opened by the `[' at START,
  // or npos.  Delegates to the shared quote-aware scanner so an escaped/quoted
  // `]' or one inside a substitution does not close the subscript.
  std::size_t matching_bracket(std::size_t start) const {
    return skip_subscript(in, start);
  }

  bool posix = false;  // posix-mode lexing (quote rules inside "${...}")
  explicit Lexer(const std::string &s) : in(s), n(s.size()) {}

  char cur() const { return in[pos]; }
  char at(std::size_t i) const { return i < n ? in[i] : '\0'; }

  void skip_blanks() {
    while (pos < n) {
      char c = in[pos];
      if (c == ' ' || c == '\t') {
        pos++;
      } else if (c == '\\' && pos + 1 < n && in[pos + 1] == '\n') {
        pos += 2;  // line continuation
      } else {
        break;
      }
    }
  }

  // -- opaque span scanners (append the span, delimiters included) ----------
  void scan_single(std::string &w) {
    int startln = line_for(pos);
    w += in[pos++];  // '
    while (pos < n && in[pos] != '\'') w += in[pos++];
    if (pos < n) w += in[pos++];
    else { unterminated = true; if (!unterm_close) { unterm_close = '\''; unterm_line = startln; } }
  }
  void scan_backtick(std::string &w) {
    int startln = line_for(pos);
    w += in[pos++];  // `
    while (pos < n && in[pos] != '`') {
      if (in[pos] == '\\') {
        w += in[pos++];
        if (pos < n) w += in[pos++];
      } else {
        w += in[pos++];
      }
    }
    if (pos < n) w += in[pos++];
    else { unterminated = true; if (!unterm_close) { unterm_close = '`'; unterm_line = startln; } }
  }
  void scan_paren(std::string &w, bool comsub_ctx = false,
                  bool array_lit = false) {  // pos at '('
    int startln = line_for(pos);  // the open line, reported for an unterminated array literal
    int depth = 0;
    struct PHd { std::string delim; bool strip; int depth; bool quoted; };
    std::vector<PHd> paren_heredocs;  // pending heredocs inside the parens
    // A `)' that terminates a `case' pattern (`case x in x)') must not be
    // mistaken for the substitution's closing paren.  Track the paren depth of
    // each active (command-position) `case' body; a `)' at that depth is a
    // pattern terminator, not the closer.  Keyword recognition is gated on
    // command position so a `case'/`esac' used as an argument is unaffected.
    bool cmd_pos = true;          // next plain word starts a command
    bool after_pipe = false;      // the previous separator was `|' (a pattern
                                  // alternative: `in|esac)' keeps its case open)
    std::vector<int> case_stack;  // paren depths of open `case' bodies
    std::string word;             // current unquoted identifier word
    bool word_plain = true;       // word is only identifier chars (a keyword?)
    bool saw_word = false;        // any word content since the last delimiter
    auto kw = [&](const std::string &w2) -> std::string {
      if (!span_aliases) return w2;
      auto it = span_aliases->find(w2);
      if (it == span_aliases->end()) return w2;
      std::string v = it->second;
      while (!v.empty() && (v.back() == ' ' || v.back() == '\t')) v.pop_back();
      size_t s = v.find_first_not_of(" \t");
      if (s != std::string::npos) v = v.substr(s);
      return (v == "case" || v == "esac") ? v : w2;
    };
    auto boundary = [&]() {
      if (saw_word) {
        // An alias whose body carries UNBALANCED parentheses moves the end of
        // the substitution (`alias nest='('', `alias short='echo ok 8 )''):
        // bash splices alias text into the input while scanning, so do the
        // same here -- replace the name in the scanned text with its body and
        // apply the paren delta.  A BALANCED body is left alone, so the
        // runtime parse performs its (single) alias expansion.
        if (cmd_pos && word_plain && span_aliases && !word.empty() &&
            w.size() >= word.size() &&
            w.compare(w.size() - word.size(), word.size(), word) == 0) {
          auto ait = span_aliases->find(word);
          if (ait != span_aliases->end()) {
            const std::string &body = ait->second;
            int delta = 0;
            bool bsq = false, bdq = false;
            for (std::size_t bi = 0; bi < body.size(); bi++) {
              char bc = body[bi];
              if (bsq) { if (bc == '\'') bsq = false; continue; }
              if (bc == '\\') { bi++; continue; }
              if (bc == '\'' && !bdq) { bsq = true; continue; }
              if (bc == '"') { bdq = !bdq; continue; }
              if (bdq) continue;
              if (bc == '(') delta++;
              else if (bc == ')') delta--;
            }
            if (delta != 0) {
              w.replace(w.size() - word.size(), word.size(), body);
              depth += delta;
            }
          }
        }
        std::string word_kw = kw(word);
        if (cmd_pos && word_plain && word_kw == "case") case_stack.push_back(depth);
        else if (word_plain && word_kw == "esac" && !case_stack.empty() && !after_pipe)
          // `esac' closes the case even out of command position: after a
          // stray `done' (`$(case x in x) ;; x) done esac)') bash's scanner
          // still ends the case body and finds the substitution's closer,
          // leaving the grammar to report the real error.  A pattern
          // ALTERNATIVE (`in|esac)') stays open.
          case_stack.pop_back();
        // Most words consume the command slot; a few reopen command position.
        cmd_pos = word_plain && (word == "then" || word == "do" ||
                                 word == "else" || word == "elif");
      }
      word.clear();
      word_plain = true;
      saw_word = false;
      after_pipe = false;
    };
    do {
      char c = in[pos];
      if (c == '(') {
        boundary();
        depth++;
        w += c;
        pos++;
        cmd_pos = true;
      } else if (c == ')') {
        boundary();
        if (!case_stack.empty() && case_stack.back() == depth) {
          w += c;  // case pattern terminator: keep depth, stay open
          pos++;
        } else {
          depth--;
          w += c;
          pos++;
        }
        cmd_pos = true;
      } else if (c == '<' && pos + 1 < n && in[pos + 1] == '<' &&
                 pos + 2 < n && in[pos + 2] == '<') {
        // `<<<' here-string: consume the whole operator so its tail is not
        // re-scanned as a here-document opener.
        w += "<<<";
        pos += 3;
        saw_word = true;
        word_plain = false;
      } else if (c == '<' && pos + 1 < n && in[pos + 1] == '<' &&
                 !(pos + 2 < n && in[pos + 2] == '<')) {
        // A here-document inside the substitution: remember its delimiter so
        // the body lines (which may contain `)') are skipped verbatim at the
        // next newline.
        bool strip_tabs = pos + 2 < n && in[pos + 2] == '-';
        w += "<<";
        pos += 2;
        if (strip_tabs) { w += '-'; pos++; }
        while (pos < n && (in[pos] == ' ' || in[pos] == '\t')) { w += in[pos]; pos++; }
        std::string delim;
        bool dquoted = false;
        while (pos < n && !std::isspace(static_cast<unsigned char>(in[pos])) &&
               !std::strchr(";&|()<>", in[pos])) {
          char dc = in[pos];
          if (dc == '\'' || dc == '"') {
            dquoted = true;
            char q = dc;
            w += in[pos++];
            while (pos < n && in[pos] != q) { delim += in[pos]; w += in[pos]; pos++; }
            if (pos < n) { w += in[pos]; pos++; }
            continue;
          }
          if (dc == '\\' && pos + 1 < n) { dquoted = true; w += dc; pos++; dc = in[pos]; }
          delim += dc;
          w += dc;
          pos++;
        }
        // Only depth 1 is a real here-document: `<<'/`>>' at depth 2 inside
        // `$((...))' are the arithmetic shifts (multiline $(( )) bodies).
        if (!delim.empty() && depth == 1)
          paren_heredocs.push_back({delim, strip_tabs, depth, dquoted});
        saw_word = true;
        word_plain = false;
      } else if (c == ';' || c == '&' || c == '|' || c == '\n') {
        boundary();
        // The splice in boundary() can supply the closing `)' from an alias
        // body (`alias short="echo ok 8 )"').  The span is finished at that
        // point, so this delimiter belongs to the text AFTER it -- consuming it
        // would pull the next command into the substitution's word.
        if (depth == 0) break;
        after_pipe = (c == '|');
        bool was_nl = c == '\n';
        w += c;
        pos++;
        cmd_pos = true;
        if (was_nl && !paren_heredocs.empty()) {
          // Copy each pending here-document body up to its delimiter line;
          // nothing in it (parens, quotes, comments) is structural.
          bool closing = false;
          for (auto &hd : paren_heredocs) {
            if (closing) break;
            while (pos < n) {
              size_t ls = pos;
              while (pos < n && in[pos] != '\n') pos++;
              std::string line = in.substr(ls, pos - ls);
              bool had_nl = pos < n;
              std::string cmp = line;
              size_t tt = 0;
              if (hd.strip)
                while (tt < cmp.size() && cmp[tt] == '\t') tt++;
              cmp = cmp.substr(tt);
              // `EOF)': the substitution's closer may abut the delimiter --
              // consume the delimiter and resume at the `)' (bash warns and
              // ends the here-document at the end of the span).
              if (cmp.compare(0, hd.delim.size(), hd.delim) == 0 &&
                  cmp.size() > hd.delim.size() && cmp[hd.delim.size()] == ')') {
                w += line.substr(0, tt + hd.delim.size());
                pos = ls + tt + hd.delim.size();
                closing = true;
                break;
              }
              w += line;
              if (had_nl) { w += '\n'; pos++; }
              if (cmp == hd.delim) break;
              if (!had_nl) {
                // Input ended inside the body: the command is incomplete and
                // the reader must know the delimiter's quoting (a trailing
                // `\' in a QUOTED body is literal, not a continuation).
                if (!heredoc_eof) {
                  heredoc_eof = true;
                  heredoc_eof_delim = hd.delim;
                  heredoc_eof_line = line_for(pos);
                  heredoc_eof_quoted = hd.quoted;
                }
                break;
              }
            }
          }
          paren_heredocs.clear();
        }
      } else if (c == ' ' || c == '\t') {
        boundary();
        if (depth == 0) break;  // an alias body supplied the closer (see above)
        w += c;
        pos++;
      } else if (c == '#' && !saw_word) {
        // A comment runs to the end of the line: a `)' in it is not the closer.
        while (pos < n && in[pos] != '\n') { w += in[pos]; pos++; }
      } else if (array_lit && c == '[' && !saw_word) {
        // A subscript at ELEMENT START of a compound assignment: bash scans
        // the `[...]' as one unit (P_ARRAYSUB), so a `)' inside the brackets
        // belongs to the subscript, not the literal -- `((X=([))]' therefore
        // runs out of input still looking for the compound's `)'
        // (parser.tests).
        saw_word = true;
        word_plain = false;
        int bdepth = 0;
        while (pos < n) {
          char bc = in[pos];
          if (bc == '\\' && pos + 1 < n) { w += bc; w += in[pos + 1]; pos += 2; continue; }
          if (bc == '\'') { scan_single(w); continue; }
          if (bc == '"') { scan_double(w); continue; }
          if (bc == '[') bdepth++;
          else if (bc == ']') {
            w += bc;
            pos++;
            if (--bdepth == 0) break;
            continue;
          }
          w += bc;
          pos++;
        }
      } else if (c == '\'') {
        saw_word = true; word_plain = false;
        scan_single(w);
      } else if (c == '"') {
        saw_word = true; word_plain = false;
        scan_double(w);
      } else if (c == '`') {
        saw_word = true; word_plain = false;
        scan_backtick(w);
      } else if (c == '\\') {
        saw_word = true; word_plain = false;
        w += c;
        pos++;
        if (pos < n) w += in[pos++];
      } else if (c == '$') {
        saw_word = true; word_plain = false;
        w += c;
        pos++;
      } else {
        saw_word = true;
        if (word_plain && (std::isalnum(static_cast<unsigned char>(c)) || c == '_'))
          word += c;
        else
          word_plain = false;
        w += c;
        pos++;
      }
    } while (pos < n && depth > 0);
    if (depth > 0) {
      unterminated = true;
      if (!unterm_close) {
        unterm_close = ')';
        // An unterminated ARRAY LITERAL reports its open line, like a quote
        // (bash parse_matched_pair start_lineno); `$(' keeps the end-of-input
        // line (parse_comsub) via unterm_line staying 0.
        if (array_lit) unterm_line = startln;
      }
      // NOTE: a here-document registered but whose BODY has not started is
      // deliberately not flagged here -- the `\' ending the delimiter word
      // itself (`<<\EOT\' + `4') is a line continuation that builds the
      // delimiter `EOT4' (comsub4.sub).  The body-skip loop above flags the
      // in-body case, which is what the reader needs.
    }
    // The substitution closed on the same line with here-documents still
    // pending: carry them out so their bodies come from the lines after the
    // full command (spliced back in before the closer), with bash's warning.
    if (comsub_ctx && depth == 0 && !paren_heredocs.empty())
      for (auto &hd : paren_heredocs)
        // Only depth-1 pendings are real here-documents: a `<<' seen at
        // depth 2 inside `$((...))' is the arithmetic left-shift.
        if (hd.depth == 1)
          comsub_carry.push_back({hd.delim, hd.strip, w.size() - 1});
  }
  void scan_brace(std::string &w, bool in_dq = false) {  // pos at '{'
    int startln = line_for(pos);
    int depth = 0;
    // Inside ${...} only a nested `${` opens another level; a bare `{` is a
    // literal character.  This mirrors bash's parse_matched_pair called with
    // P_FIRSTCLOSE for ${...}: it counts `{` only when the previous char was
    // an unquoted `$` (LEX_WASDOL).  `wasdol` tracks that.
    //
    // Exception: a funsub `${ cmd; }' (whitespace or `|' right after the brace)
    // holds a command list, so bare `{' (group commands, function bodies)
    // balance normally -- otherwise a function body's `}' closes the scan early.
    bool wasdol = false;
    bool funsub = pos + 1 < n &&
                  (std::isspace(static_cast<unsigned char>(in[pos + 1])) || in[pos + 1] == '|');
    do {
      char c = in[pos];
      if (c == '{') {
        if (depth == 0 || wasdol || funsub) depth++;  // opening ${, nested ${, or funsub group
        w += c;
        pos++;
        wasdol = false;
      } else if (c == '}') {
        depth--;
        w += c;
        pos++;
        wasdol = false;
      } else if (c == '$' && pos + 1 < n && in[pos + 1] == '\'') {
        scan_dollar_single(w);  // $'...' inside ${...}: backslash-aware
        wasdol = false;
      } else if (c == '$' && pos + 1 < n && in[pos + 1] == '(') {
        // A nested command/arith substitution `$( ... )' / `$(( ... ))' is
        // scanned by paren balancing so any `{'/`}' inside it are consumed as
        // its content rather than counted against this ${...}'s brace depth.
        w += c;
        pos++;  // the `$'
        scan_paren(w);
        wasdol = false;
      } else if (c == '\'') {
        // POSIX mode: inside a DOUBLE-QUOTED ${...} a single quote is an
        // ordinary character, not a quote (`"${IFS+'}'z}"` ends at the first
        // `}`), matching bash's parse_matched_pair posix handling.
        if (posix && in_dq) { w += c; pos++; }
        else scan_single(w);
        wasdol = false;
      } else if (c == '"') {
        scan_double(w);
        wasdol = false;
      } else if (c == '`') {
        scan_backtick(w);
        wasdol = false;
      } else if (c == '\\') {
        w += c;
        pos++;
        if (pos < n) w += in[pos++];
        wasdol = false;
      } else {
        wasdol = (c == '$');
        w += c;
        pos++;
      }
    } while (pos < n && depth > 0);
    if (depth > 0) { unterminated = true; if (!unterm_close) { unterm_close = '}'; unterm_line = startln; } }
  }
  void scan_square(std::string &w) {  // pos at '[' of $[...] arithmetic
    int depth = 0;
    do {
      char c = in[pos];
      if (c == '[') {
        depth++;
        w += c;
        pos++;
      } else if (c == ']') {
        depth--;
        w += c;
        pos++;
      } else if (c == '\'') {
        scan_single(w);
      } else if (c == '"') {
        scan_double(w);
      } else if (c == '`') {
        scan_backtick(w);
      } else if (c == '\\') {
        w += c;
        pos++;
        if (pos < n) w += in[pos++];
      } else {
        w += c;
        pos++;
      }
    } while (pos < n && depth > 0);
    if (depth > 0) { unterminated = true; if (!unterm_close) unterm_close = ']'; }
  }
  void scan_double(std::string &w) {
    int startln = line_for(pos);
    w += in[pos++];  // "
    while (pos < n && in[pos] != '"') {
      char c = in[pos];
      if (c == '\\') {
        w += c;
        pos++;
        if (pos < n) w += in[pos++];
      } else if (c == '`') {
        scan_backtick(w);
      } else if (c == '$' && pos + 1 < n && in[pos + 1] == '(') {
        w += '$';
        pos++;
        scan_paren(w);
      } else if (c == '$' && pos + 1 < n && in[pos + 1] == '{') {
        w += '$';
        pos++;
        scan_brace(w, /*in_dq=*/true);
      } else {
        w += c;
        pos++;
      }
    }
    if (pos < n) w += in[pos++];
    else { unterminated = true; if (!unterm_close) { unterm_close = '"'; unterm_line = startln; } }
  }
  void scan_dollar_single(std::string &w) {  // pos at '$', next '\''
    int startln = line_for(pos);
    w += in[pos++];  // $
    w += in[pos++];  // '
    while (pos < n && in[pos] != '\'') {
      if (in[pos] == '\\') {
        w += in[pos++];
        if (pos < n) w += in[pos++];
      } else {
        w += in[pos++];
      }
    }
    if (pos < n) w += in[pos++];
    else { unterminated = true; if (!unterm_close) { unterm_close = '\''; unterm_line = startln; } }
  }

  bool is_metachar(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '|' || c == '&' ||
           c == ';' || c == '(' || c == ')' || c == '<' || c == '>';
  }

  // True when a `name=(...)' compound may be recognized at the CURRENT word:
  // the current simple command is still in assignment-prefix position (every
  // word so far is an assignment), or its command word is an assignment
  // builtin (declare/typeset/local/export/readonly/alias) -- options and
  // redirections in between don't matter.  After an ordinary command word the
  // `(' stays unconsumed and the parser reports bash's syntax error
  // (`printf "%s\n" -a a=(a 'b  c')', array1.sub).
  bool assignment_acceptable() const {
    static const std::set<std::string> kAssignBuiltins = {
        "declare", "typeset", "local", "export", "readonly", "alias",
        "eval", "let"};  // bash marks eval/let assignment builtins too
    static const std::set<std::string> kCmdStart = {
        "if", "then", "else", "elif", "fi", "do", "done", "while", "until",
        "for", "in", "case", "esac", "{", "}", "!", "time", "coproc", "function"};
    auto is_sep = [](Tok ty) {
      switch (ty) {
        case Tok::Newline: case Tok::Amp: case Tok::Semi: case Tok::Pipe:
        case Tok::AndAnd: case Tok::OrOr: case Tok::SemiSemi: case Tok::SemiAnd:
        case Tok::SemiSemiAnd: case Tok::PipeAnd: case Tok::Lparen: case Tok::Rparen:
          return true;
        default:
          return false;
      }
    };
    auto assign_shaped = [](const std::string &s) {
      size_t e = s.find('=');
      if (e == std::string::npos || e == 0) return false;
      if (!(std::isalpha(static_cast<unsigned char>(s[0])) || s[0] == '_')) return false;
      size_t k = 0;
      while (k < e && (std::isalnum(static_cast<unsigned char>(s[k])) || s[k] == '_')) k++;
      if (k < e && s[k] == '[') {
        size_t rb = s.rfind(']', e);
        k = (rb != std::string::npos && rb < e) ? rb + 1 : k;
      }
      if (k < e && s[k] == '+') k++;
      return k == e;
    };
    size_t start = out.size();
    while (start > 0 && !is_sep(out[start - 1].type)) start--;
    bool redir_target = false;
    for (size_t k = start; k < out.size(); k++) {
      const Token &t = out[k];
      if (t.type == Tok::IoNumber) continue;
      if (t.type != Tok::Word) { redir_target = true; continue; }  // a redir operator
      if (redir_target) { redir_target = false; continue; }        // ... and its target
      if (kCmdStart.count(t.text)) continue;  // reserved word: command not started yet
      if (kAssignBuiltins.count(t.text)) return true;
      if (assign_shaped(t.text)) continue;    // a prefix assignment keeps the position
      return false;                           // an ordinary command word ends it
    }
    return true;  // still in command-prefix position
  }

  // Bytes of the multibyte character starting at `pos' (1 for ASCII, invalid
  // sequences, or single-byte locales).  In charsets like Big5 a trail byte
  // can be `\' or a metacharacter value, so word scanning must consume a
  // multibyte character atomically or the trail byte is misread as syntax.
  std::size_t mb_len() const {
    if (static_cast<unsigned char>(in[pos]) < 0x80 || MB_CUR_MAX <= 1) return 1;
    wchar_t wc;
    std::mbstate_t st{};
    std::size_t r = std::mbrtowc(&wc, in.data() + pos, n - pos, &st);
    return (r == static_cast<std::size_t>(-1) || r == static_cast<std::size_t>(-2) || r == 0)
               ? 1
               : r;
  }

  Token read_word() {
    std::string w;
    bool quoted = false;
    while (pos < n) {
      char c = in[pos];
      if (c == ' ' || c == '\t' || c == '\n') break;
      if (static_cast<unsigned char>(c) >= 0x80) {
        std::size_t l = mb_len();
        w.append(in, pos, l);
        pos += l;
        continue;
      }
      if ((c == '<' || c == '>') && pos + 1 < n && in[pos + 1] == '(') {
        w += c;
        pos++;
        scan_paren(w);  // process substitution
        continue;
      }
      if (c == '(' && is_assignment_prefix(w)) {
        // A compound value is only recognized where an ASSIGNMENT can appear:
        // in command-prefix position or after an assignment builtin.  After an
        // ordinary command word the `(' is a hard syntax error, exactly as
        // bash's parser treats `printf "%s\n" -a a=(a 'b  c')' (array1.sub);
        // leaving it unconsumed makes it an operator token the parser rejects.
        if (!assignment_acceptable()) break;
        scan_paren(w, false, true);  // compound (array) assignment value: name=(...)
        quoted = true;
        continue;
      }
      // An array-assignment subscript `name[...]=' / `name[...]+=' is one word,
      // spaces and all (`a[7 + 8]=v'); a bare `name[...]' (no `=') is not -- so
      // `set -- a[1 2]' still splits.  Only scan when a `=' follows the `]'.
      if (c == '[' && is_name_word(w)) {
        std::size_t close = matching_bracket(pos);
        std::size_t after = close == std::string::npos ? n : close + 1;
        bool assign = after < n && (in[after] == '=' ||
                      (in[after] == '+' && after + 1 < n && in[after + 1] == '='));
        if (assign) {
          w.append(in, pos, close - pos + 1);
          pos = close + 1;
          continue;
        }
      }
      // Extended-glob operator: ?(...) *(...) +(...) @(...) !(...)
      if ((c == '?' || c == '*' || c == '+' || c == '@' || c == '!') &&
          pos + 1 < n && in[pos + 1] == '(') {
        w += c;
        pos++;
        scan_paren(w);
        continue;
      }
      if (is_metachar(c)) break;
      if (c == '\\') {
        if (pos + 1 < n && in[pos + 1] == '\n') {
          pos += 2;
          continue;
        }
        w += c;
        pos++;
        if (pos < n) w += in[pos++];
        quoted = true;
        continue;
      }
      if (c == '\'') {
        scan_single(w);
        quoted = true;
        continue;
      }
      if (c == '"') {
        scan_double(w);
        quoted = true;
        continue;
      }
      if (c == '`') {
        scan_backtick(w);
        continue;
      }
      if (c == '$') {
        if (pos + 1 < n && in[pos + 1] == '\'') {
          scan_dollar_single(w);
          quoted = true;
          continue;
        }
        if (pos + 1 < n && in[pos + 1] == '"') {
          w += '$';
          pos++;
          scan_double(w);
          quoted = true;
          continue;
        }
        if (pos + 1 < n && in[pos + 1] == '(') {
          w += '$';
          pos++;
          scan_paren(w, /*comsub_ctx=*/true);
          continue;
        }
        if (pos + 1 < n && in[pos + 1] == '{') {
          w += '$';
          pos++;
          scan_brace(w);
          continue;
        }
        if (pos + 1 < n && in[pos + 1] == '[') {
          w += '$';
          pos++;
          scan_square(w);  // $[...] deprecated arithmetic: span internal spaces
          continue;
        }
        w += c;
        pos++;
        continue;
      }
      w += c;
      pos++;
    }
    Token t;
    t.text = w;
    t.quoted = quoted;
    bool all_digits = !w.empty() && !quoted;
    for (char c : w)
      if (!std::isdigit(static_cast<unsigned char>(c))) all_digits = false;
    if (all_digits && pos < n && (in[pos] == '<' || in[pos] == '>')) {
      // bash: a number too large for an int is not a file descriptor -- the
      // digits stay a command WORD, so `$(11111111111111111111</dev/stdin)'
      // runs (and fails to find) that command (new-exp2.sub).
      errno = 0;
      long long fv = std::strtoll(w.c_str(), nullptr, 10);
      t.type = (errno == 0 && fv <= 2147483647LL) ? Tok::IoNumber : Tok::Word;
    } else {
      t.type = Tok::Word;
    }
    return t;
  }

  Token read_operator() {
    Token t;
    char c = in[pos];
    switch (c) {
      case '(':
        t.type = Tok::Lparen;
        pos++;
        t.glued = (pos < n && in[pos] == '(');
        break;
      case ')': t.type = Tok::Rparen; pos++; break;
      case '|':
        if (at(pos + 1) == '|') { t.type = Tok::OrOr; pos += 2; }
        else if (at(pos + 1) == '&') { t.type = Tok::PipeAnd; pos += 2; }
        else { t.type = Tok::Pipe; pos++; }
        break;
      case '&':
        if (at(pos + 1) == '&') { t.type = Tok::AndAnd; pos += 2; }
        else if (at(pos + 1) == '>' && at(pos + 2) == '>') { t.type = Tok::AndDGreat; pos += 3; }
        else if (at(pos + 1) == '>') { t.type = Tok::AndGreat; pos += 2; }
        else { t.type = Tok::Amp; pos++; }
        break;
      case ';':
        if (at(pos + 1) == ';' && at(pos + 2) == '&') { t.type = Tok::SemiSemiAnd; pos += 3; }
        else if (at(pos + 1) == ';') { t.type = Tok::SemiSemi; pos += 2; }
        else if (at(pos + 1) == '&') { t.type = Tok::SemiAnd; pos += 2; }
        else { t.type = Tok::Semi; pos++; }
        break;
      case '<':
        if (at(pos + 1) == '<' && at(pos + 2) == '-') { t.type = Tok::DLessDash; pos += 3; }
        else if (at(pos + 1) == '<' && at(pos + 2) == '<') { t.type = Tok::TLess; pos += 3; }
        else if (at(pos + 1) == '<') { t.type = Tok::DLess; pos += 2; }
        else if (at(pos + 1) == '&') { t.type = Tok::LessAnd; pos += 2; }
        else if (at(pos + 1) == '>') { t.type = Tok::LessGreat; pos += 2; }
        else { t.type = Tok::Less; pos++; }
        break;
      case '>':
        if (at(pos + 1) == '>') { t.type = Tok::DGreat; pos += 2; }
        else if (at(pos + 1) == '&') { t.type = Tok::GreatAnd; pos += 2; }
        else if (at(pos + 1) == '|') { t.type = Tok::Clobber; pos += 2; }
        else { t.type = Tok::Great; pos++; }
        break;
      default: t.type = Tok::Eof; pos++; break;
    }
    return t;
  }

  std::string dequote_delim(const std::string &d, bool &quoted) {
    std::string out_s;
    quoted = false;
    for (std::size_t i = 0; i < d.size(); i++) {
      char c = d[i];
      if (c == '\'' || c == '"') {
        quoted = true;
      } else if (c == '\\') {
        quoted = true;
        if (i + 1 < d.size()) out_s += d[++i];
      } else {
        out_s += c;
      }
    }
    return out_s;
  }

  void collect_heredocs() {
    // The line whose newline TRIGGERED gathering -- bash's warning names it
    // ("here-document at line %d delimited by end-of-file"): the redirect's
    // own line for a one-line command, but the line of the `)' for
    // `cat <<EOF && grep $(...\n...)' (heredoc7.sub).
    int gather_line = line_for(pos > 0 ? pos - 1 : 0);
    for (Pending &pd : pending) {
      std::string body;
      bool found = false;
      while (pos < n) {
        std::size_t ls = pos;
        while (pos < n && in[pos] != '\n') pos++;
        std::string line = in.substr(ls, pos - ls);
        bool had_nl = pos < n;
        if (had_nl) pos++;  // consume newline

        std::string cmp = line;
        std::string stored = line;
        if (pd.strip) {
          std::size_t t = 0;
          while (t < cmp.size() && cmp[t] == '\t') t++;
          cmp = cmp.substr(t);
          stored = cmp;
        }
        std::string want = pd.delim;
        if (pd.strip) {  // `<<-' tab-strips the delimiter as well (`<<-'\tEND'')
          std::size_t t = 0;
          while (t < want.size() && want[t] == '\t') t++;
          want = want.substr(t);
        }
        if (cmp == want) {
          // A delimiter with no trailing newline (`...\nEOF' at the end of a
          // command substitution) ends the body but bash still reports it as
          // delimited by end-of-file.
          if (had_nl) found = true;
          break;
        }
        body += stored;
        body += '\n';
        if (!had_nl) break;  // EOF before delimiter
      }
      if (!found && !heredoc_eof) {
        // Delimiter never seen: end-of-input delimits the body (bash warns).
        // Line readers treat this as incomplete and keep accumulating input.
        // A regular here-document names the line whose newline triggered
        // gathering; one embedded in a command substitution was gathered at
        // parse time within the substitution and keeps its redirect's line.
        heredoc_eof = true;
        heredoc_eof_delim = pd.delim;
        heredoc_eof_line = pd.comsub ? out[pd.index].line : gather_line;
        heredoc_eof_quoted = pd.quoted;
      }
      if (pd.comsub) {
        // Splice the body (and its delimiter line) back into the word before
        // the substitution's closer: the inner command reads it when it runs.
        std::string ins = std::string(1, '\n') + body + pd.delim + '\n';
        out[pd.index].text.insert(pd.splice_at, ins);
        for (Pending &later : pending)
          if (&later > &pd && later.comsub && later.index == pd.index)
            later.splice_at += ins.size();
        continue;
      }
      out[pd.index].heredoc_body = body;
      out[pd.index].has_heredoc = true;
      out[pd.index].heredoc_quoted = pd.quoted;
    }
    pending.clear();
  }

  void run() {
    while (true) {
      std::size_t bpos = pos;
      skip_blanks();
      bool blanked = (pos != bpos);
      if (pos >= n) break;
      int tline = line_for(pos);
      char c = in[pos];

      if (c == '\n') {
        Token t;
        t.type = Tok::Newline;
        t.line = tline;
        t.preceded_by_blank = blanked;
        t.start = pos;
        t.end = pos + 1;
        out.push_back(t);
        pos++;
        if (!pending.empty()) collect_heredocs();
        awaiting = -1;
        cmd_heredocs = 0;  // need_here_doc resets once the bodies are gathered
        continue;
      }
      if (c == '#') {  // comment to end of line
        while (pos < n && in[pos] != '\n') pos++;
        continue;
      }

      bool is_op = false;
      if (c == '|' || c == '&' || c == ';' || c == '(' || c == ')') is_op = true;
      if ((c == '<' || c == '>') && !(pos + 1 < n && in[pos + 1] == '(')) is_op = true;

      if (is_op) {
        std::size_t tstart = pos;
        Token t = read_operator();
        t.line = tline;
        t.preceded_by_blank = blanked;
        t.start = tstart;
        t.end = pos;
        bool is_heredoc = (t.type == Tok::DLess || t.type == Tok::DLessDash);
        out.push_back(t);
        if (is_heredoc) awaiting = (t.type == Tok::DLessDash) ? 1 : 0;
        continue;
      }

      std::size_t wstart = pos;
      Token t = read_word();
      t.line = tline;
      t.preceded_by_blank = blanked;
      t.start = wstart;
      t.end = pos;
      out.push_back(t);
      if (!comsub_carry.empty()) {
        for (auto &ch : comsub_carry) {
          pending.push_back({out.size() - 1, ch.delim, ch.strip, /*quoted=*/false,
                             /*comsub=*/true, ch.splice_at});
          comsub_unterm++;
        }
        comsub_unterm_line = t.line;
        comsub_carry.clear();
      }
      if (awaiting >= 0 && t.type == Tok::Word) {
        bool q = false;
        std::string d = dequote_delim(t.text, q);
        pending.push_back({out.size() - 1, d, awaiting == 1, q});
        awaiting = -1;
        // bash's push_heredoc: the 17th here-document on one command is a
        // fatal syntax error (HEREDOC_MAX == 16).
        if (++cmd_heredocs > 16 && !heredoc_overflow) {
          heredoc_overflow = true;
          heredoc_overflow_line = t.line;
        }
      }
    }
    // A here-doc redirection with no newline after it (input ended on the
    // command line): collect now so the empty body and the end-of-input
    // condition are recorded.
    if (!pending.empty()) collect_heredocs();
    Token eof;
    eof.type = Tok::Eof;
    eof.start = n;
    eof.end = n;
    // bash parses input with a guaranteed trailing newline, so EOF falls on the
    // line after the last content -- add one when the input has no final newline.
    eof.line = line_for(n) + ((n > 0 && in[n - 1] != '\n') ? 1 : 0);
    eof.lex_error = unterminated;
    eof.lex_close = unterm_close;
    eof.lex_open_line = unterm_line;
    eof.heredoc_eof = heredoc_eof;
    eof.heredoc_eof_delim = heredoc_eof_delim;
    eof.heredoc_eof_line = heredoc_eof_line;
    eof.heredoc_eof_quoted = heredoc_eof_quoted;
    eof.comsub_unterm = comsub_unterm;
    eof.comsub_unterm_line = comsub_unterm_line;
    eof.heredoc_overflow = heredoc_overflow;
    eof.heredoc_overflow_line = heredoc_overflow_line;
    out.push_back(eof);
  }
};

}  // namespace

// Offset of the character just past the `)' closing the substitution whose
// `(' is at OPEN_POS (quote-, case-pattern-, comment-, and heredoc-aware --
// the lexer's own scanner).  npos when unterminated.
std::size_t comsub_span_end(const std::string &text, std::size_t open_pos) {
  Lexer lx{text};
  lx.pos = open_pos;
  std::string dummy;
  lx.scan_paren(dummy);
  return lx.unterminated ? std::string::npos : lx.pos;
}

std::size_t comsub_span_end_aliased(const std::string &text, std::size_t open_pos,
                                    const std::map<std::string, std::string> &aliases) {
  Lexer lx{text};
  lx.pos = open_pos;
  lx.span_aliases = &aliases;
  std::string dummy;
  lx.scan_paren(dummy);
  return lx.unterminated ? std::string::npos : lx.pos;
}


std::vector<Token> tokenize(const std::string &input, bool posix_mode,
                            const std::map<std::string, std::string> *span_aliases,
                            const std::vector<int> *cont_lines) {
  Lexer lx(input);
  lx.posix = posix_mode;
  lx.span_aliases = span_aliases;
  lx.cont_lines = cont_lines;
  lx.run();
  return std::move(lx.out);
}

}  // namespace gnash::core
