// Copyright (c) 2026 Brian J. Fox
// Licensed under GPLv2 with the GPLv2-AI Exception.

// lexer.hpp -- shell tokenizer.
//
// Splits shell input into the token stream the parser consumes, following the
// word-boundary and quoting rules from bash 5.3's parse.y lexer: single/double/
// ANSI-C quotes, $(...)/`...`/${...} spans kept opaque (parsed later during
// expansion), operator recognition, IO numbers, comments, line continuation,
// and here-document body collection.
#ifndef GNASH_CORE_LEXER_HPP
#define GNASH_CORE_LEXER_HPP

#include <map>
#include <string>
#include <vector>

namespace gnash::core {

enum class Tok {
  Word,
  IoNumber,
  Newline,
  Amp,          // &
  Semi,         // ;
  Pipe,         // |
  AndAnd,       // &&
  OrOr,         // ||
  SemiSemi,     // ;;
  SemiAnd,      // ;&
  SemiSemiAnd,  // ;;&
  PipeAnd,      // |&
  Lparen,       // (
  Rparen,       // )
  Less,         // <
  Great,        // >
  DLess,        // <<
  DGreat,       // >>
  DLessDash,    // <<-
  TLess,        // <<<
  LessAnd,      // <&
  GreatAnd,     // >&
  LessGreat,    // <>
  Clobber,      // >|
  AndGreat,     // &>
  AndDGreat,    // &>>
  Eof,
};

struct Token {
  Tok type = Tok::Eof;
  std::string text;             // for Word / IoNumber
  int line = 1;                 // 1-based source line where the token starts
  std::size_t start = 0;        // byte offset of the token in the input
  std::size_t end = 0;          // one past the token's last byte
  bool preceded_by_blank = true;  // whitespace separated this from the previous token
  bool quoted = false;          // word contained quoting
  bool glued = false;           // Lparen immediately followed by `(' (for `((')
  bool lex_error = false;       // set on the Eof token if a span was unterminated
  char lex_close = 0;           // the closer we were scanning for at EOF
  // Set on the Eof token when a here-document ran to end-of-input without its
  // delimiter: the body is still attached (bash runs it with a warning), but
  // line-oriented readers treat the input as incomplete.
  bool heredoc_eof = false;
  std::string heredoc_eof_delim;
  int heredoc_eof_line = 0;
  bool heredoc_eof_quoted = false;  // the open here-doc's delimiter was quoted
  // Here-documents left pending when a $(...) closed on the same line: bash
  // warns and takes their bodies from the lines after the full command.
  int comsub_unterm = 0;
  int comsub_unterm_line = 0;
  // For a here-document delimiter word, the collected body and whether the
  // delimiter was quoted (which disables expansion of the body).
  std::string heredoc_body;
  bool has_heredoc = false;
  bool heredoc_quoted = false;
};

const char *tok_name(Tok t);

// Tokenize INPUT.  Always ends with a single Tok::Eof token.  Unterminated
// quotes/spans are tolerated (consumed to end of input); the parser decides
// whether the result is a syntax error.
// Offset just past the `)' closing the substitution whose `(' is at open_pos
// (quote/case/comment/heredoc-aware); npos when unterminated.
std::size_t comsub_span_end(const std::string &text, std::size_t open_pos);

// Same, but resolving one level of ALIAS for the `case'/`esac' keywords: bash
// expands aliases while scanning substitution content, so `alias switch=case'
// makes `$( switch x in y) ...;; esac )' scan correctly (comsub5.sub).  Pass
// the shell's alias table; names whose expansion is exactly `case' or `esac'
// are treated as that keyword.
std::size_t comsub_span_end_aliased(const std::string &text, std::size_t open_pos,
                                    const std::map<std::string, std::string> &aliases);

// CONT_LINES lists the line numbers of INPUT at which a `\'-newline line
// continuation was spliced away before parsing.  Each one makes every later
// line of the source one higher than the text shows, so token line numbers add
// them back.  Without it a continued line inside a compound command shifts the
// whole rest of the body (`$LINENO', the DEBUG trap, BASH_LINENO).
std::vector<Token> tokenize(const std::string &input, bool posix_mode = false,
                            const std::map<std::string, std::string> *span_aliases = nullptr,
                            const std::vector<int> *cont_lines = nullptr);

}  // namespace gnash::core

#endif  // GNASH_CORE_LEXER_HPP
