// Copyright (c) 2026 Brian J. Fox
// Licensed under GPLv2 with the GPLv2-AI Exception.

// lexer.cpp -- shell tokenizer (see lexer.hpp).

#include "gnash/core/lexer.hpp"

#include <cctype>

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
};

struct Lexer {
  const std::string &in;
  std::size_t pos = 0;
  std::size_t n;
  std::vector<Token> out;
  std::vector<Pending> pending;
  int awaiting = -1;  // -1 none, 0 <<, 1 <<-
  bool unterminated = false;
  char unterm_close = 0;  // the closer we were looking for at EOF
  bool heredoc_eof = false;        // here-doc body delimited by end of input
  std::string heredoc_eof_delim;
  int heredoc_eof_line = 0;
  std::size_t line_scanned = 0;  // bytes already counted for line numbering
  int cur_line = 1;              // 1-based line at line_scanned

  // Line number of the byte at `start` (pos advances monotonically).
  int line_for(std::size_t start) {
    while (line_scanned < start && line_scanned < n) {
      if (in[line_scanned] == '\n') cur_line++;
      line_scanned++;
    }
    return cur_line;
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

  // Offset of the `]' matching the `[' at START (handling nesting), or npos.
  std::size_t matching_bracket(std::size_t start) const {
    int depth = 0;
    for (std::size_t k = start; k < n; k++) {
      if (in[k] == '[') depth++;
      else if (in[k] == ']' && --depth == 0) return k;
      else if (in[k] == '\n') break;  // a subscript does not span input lines here
    }
    return std::string::npos;
  }

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
    w += in[pos++];  // '
    while (pos < n && in[pos] != '\'') w += in[pos++];
    if (pos < n) w += in[pos++];
    else { unterminated = true; if (!unterm_close) unterm_close = '\''; }
  }
  void scan_backtick(std::string &w) {
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
    else { unterminated = true; if (!unterm_close) unterm_close = '`'; }
  }
  void scan_paren(std::string &w) {  // pos at '('
    int depth = 0;
    do {
      char c = in[pos];
      if (c == '(') {
        depth++;
        w += c;
        pos++;
      } else if (c == ')') {
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
    if (depth > 0) { unterminated = true; if (!unterm_close) unterm_close = ')'; }
  }
  void scan_brace(std::string &w) {  // pos at '{'
    int depth = 0;
    do {
      char c = in[pos];
      if (c == '{') {
        depth++;
        w += c;
        pos++;
      } else if (c == '}') {
        depth--;
        w += c;
        pos++;
      } else if (c == '$' && pos + 1 < n && in[pos + 1] == '\'') {
        scan_dollar_single(w);  // $'...' inside ${...}: backslash-aware
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
    if (depth > 0) { unterminated = true; if (!unterm_close) unterm_close = '}'; }
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
        scan_brace(w);
      } else {
        w += c;
        pos++;
      }
    }
    if (pos < n) w += in[pos++];
    else { unterminated = true; if (!unterm_close) unterm_close = '"'; }
  }
  void scan_dollar_single(std::string &w) {  // pos at '$', next '\''
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
    else { unterminated = true; if (!unterm_close) unterm_close = '\''; }
  }

  bool is_metachar(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '|' || c == '&' ||
           c == ';' || c == '(' || c == ')' || c == '<' || c == '>';
  }

  Token read_word() {
    std::string w;
    bool quoted = false;
    while (pos < n) {
      char c = in[pos];
      if (c == ' ' || c == '\t' || c == '\n') break;
      if ((c == '<' || c == '>') && pos + 1 < n && in[pos + 1] == '(') {
        w += c;
        pos++;
        scan_paren(w);  // process substitution
        continue;
      }
      if (c == '(' && is_assignment_prefix(w)) {
        scan_paren(w);  // compound (array) assignment value: name=(...)
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
          scan_paren(w);
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
    if (all_digits && pos < n && (in[pos] == '<' || in[pos] == '>'))
      t.type = Tok::IoNumber;
    else
      t.type = Tok::Word;
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
        if (cmp == pd.delim) { found = true; break; }  // terminator line
        body += stored;
        body += '\n';
        if (!had_nl) break;  // EOF before delimiter
      }
      if (!found && !heredoc_eof) {
        // Delimiter never seen: end-of-input delimits the body (bash warns).
        // Line readers treat this as incomplete and keep accumulating input.
        heredoc_eof = true;
        heredoc_eof_delim = pd.delim;
        heredoc_eof_line = out[pd.index].line;
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
        out.push_back(t);
        pos++;
        if (!pending.empty()) collect_heredocs();
        awaiting = -1;
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
        Token t = read_operator();
        t.line = tline;
        t.preceded_by_blank = blanked;
        bool is_heredoc = (t.type == Tok::DLess || t.type == Tok::DLessDash);
        out.push_back(t);
        if (is_heredoc) awaiting = (t.type == Tok::DLessDash) ? 1 : 0;
        continue;
      }

      Token t = read_word();
      t.line = tline;
      t.preceded_by_blank = blanked;
      out.push_back(t);
      if (awaiting >= 0 && t.type == Tok::Word) {
        bool q = false;
        std::string d = dequote_delim(t.text, q);
        pending.push_back({out.size() - 1, d, awaiting == 1, q});
        awaiting = -1;
      }
    }
    // A here-doc redirection with no newline after it (input ended on the
    // command line): collect now so the empty body and the end-of-input
    // condition are recorded.
    if (!pending.empty()) collect_heredocs();
    Token eof;
    eof.type = Tok::Eof;
    // bash parses input with a guaranteed trailing newline, so EOF falls on the
    // line after the last content -- add one when the input has no final newline.
    eof.line = line_for(n) + ((n > 0 && in[n - 1] != '\n') ? 1 : 0);
    eof.lex_error = unterminated;
    eof.lex_close = unterm_close;
    eof.heredoc_eof = heredoc_eof;
    eof.heredoc_eof_delim = heredoc_eof_delim;
    eof.heredoc_eof_line = heredoc_eof_line;
    out.push_back(eof);
  }
};

}  // namespace

std::vector<Token> tokenize(const std::string &input) {
  Lexer lx(input);
  lx.run();
  return std::move(lx.out);
}

}  // namespace gnash::core
