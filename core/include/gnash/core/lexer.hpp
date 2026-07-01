// lexer.hpp -- shell tokenizer.
//
// Splits shell input into the token stream the parser consumes, following the
// word-boundary and quoting rules from bash 5.3's parse.y lexer: single/double/
// ANSI-C quotes, $(...)/`...`/${...} spans kept opaque (parsed later during
// expansion), operator recognition, IO numbers, comments, line continuation,
// and here-document body collection.
#ifndef GNASH_CORE_LEXER_HPP
#define GNASH_CORE_LEXER_HPP

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
  bool preceded_by_blank = true;  // whitespace separated this from the previous token
  bool quoted = false;          // word contained quoting
  bool glued = false;           // Lparen immediately followed by `(' (for `((')
  bool lex_error = false;       // set on the Eof token if a span was unterminated
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
std::vector<Token> tokenize(const std::string &input);

}  // namespace gnash::core

#endif  // GNASH_CORE_LEXER_HPP
