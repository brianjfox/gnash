// ast.hpp -- the shell command AST.
//
// C++ analogue of bash 5.3 command.h.  Rather than a tagged union we use a
// small class hierarchy of owned nodes; the node kinds and their fields mirror
// bash's COMMAND / WORD_DESC / REDIRECT so behaviour can be matched precisely.
#ifndef GNASH_CORE_AST_HPP
#define GNASH_CORE_AST_HPP

#include <memory>
#include <string>
#include <vector>

namespace gnash::core {

// ---- words ----------------------------------------------------------------

// WORD_DESC flags (subset of bash's).
enum WordFlag {
  W_QUOTED = 0x01,      // contained a quoted substring
  W_ASSIGNMENT = 0x02,  // looks like `name=value'
  W_HASDOLLAR = 0x04,   // contains an unquoted `$'
};

struct Word {
  std::string text;
  int flags = 0;
  Word() = default;
  Word(std::string t, int f = 0) : text(std::move(t)), flags(f) {}
};

// ---- redirections ---------------------------------------------------------

enum class RedirOp {
  InputRedir,     // <
  OutputRedir,    // >
  AppendOutput,   // >>
  Clobber,        // >|
  InputOutput,    // <>
  DupInput,       // <&
  DupOutput,      // >&
  HereString,     // <<<
  HereDoc,        // <<
  HereDocStrip,   // <<-
  AndOutput,      // &>
  AndAppend,      // &>>
};

struct Redirect {
  int source_fd = -1;   // -1 => operator default (0 for input, 1 for output)
  RedirOp op = RedirOp::OutputRedir;
  Word target;          // filename, fd word, or here-doc delimiter
  std::string heredoc_body;
  bool heredoc_quoted = false;  // delimiter was quoted => no expansion
};

// Command flags (subset of bash's).
enum CommandFlag {
  CMD_INVERT_RETURN = 0x01,  // leading `!'
  CMD_TIME = 0x02,           // `time'
  CMD_TIME_POSIX = 0x04,     // `time -p'
};

// ---- commands -------------------------------------------------------------

struct Command {
  int flags = 0;
  std::vector<Redirect> redirects;
  virtual ~Command() = default;
  // Append a canonical rendering of this command to `out`.
  virtual void print(std::string &out) const = 0;
};

using CommandPtr = std::unique_ptr<Command>;

std::string to_string(const Command *c);  // full canonical rendering

struct SimpleCommand : Command {
  std::vector<Word> words;  // assignment prefix + argv, in order
  void print(std::string &out) const override;
};

enum class Connector { And, Or, Semi, Amp, Pipe, Newline };

struct Connection : Command {
  Connector conn = Connector::Semi;
  CommandPtr first;
  CommandPtr second;  // may be null (e.g. trailing `&')
  void print(std::string &out) const override;
};

struct Subshell : Command {  // ( list )
  CommandPtr body;
  void print(std::string &out) const override;
};

struct Group : Command {  // { list; }
  CommandPtr body;
  void print(std::string &out) const override;
};

struct IfCommand : Command {
  CommandPtr cond;
  CommandPtr then_part;
  CommandPtr else_part;  // may be null; nested IfCommand for `elif'
  void print(std::string &out) const override;
};

struct LoopCommand : Command {  // while / until
  bool until = false;
  CommandPtr cond;
  CommandPtr body;
  void print(std::string &out) const override;
};

struct ForCommand : Command {
  bool is_select = false;      // `select' instead of `for'
  bool is_arith = false;       // `for (( init; cond; update ))'
  std::string var;             // for / select variable
  std::vector<Word> words;     // the `in' list
  bool words_present = false;  // distinguishes `for x' from `for x in' (empty)
  std::string a_init, a_cond, a_update;  // arithmetic-for expressions
  CommandPtr body;
  void print(std::string &out) const override;
};

struct CondCommand : Command {  // [[ expression ]]
  std::string expression;       // reconstructed, space-separated
  void print(std::string &out) const override;
};

struct ArithCommand : Command {  // (( expression ))
  std::string expression;
  void print(std::string &out) const override;
};

struct CoprocCommand : Command {  // coproc [NAME] command
  std::string name;
  CommandPtr body;
  void print(std::string &out) const override;
};

struct CaseClause {
  std::vector<Word> patterns;
  CommandPtr body;       // may be null (empty clause)
  int terminator = 0;    // ';;' (0), ';&', or ';;&' -- token id from parser
};

struct CaseCommand : Command {
  Word word;
  std::vector<CaseClause> clauses;
  void print(std::string &out) const override;
};

struct FunctionDef : Command {
  std::string name;
  CommandPtr body;
  void print(std::string &out) const override;
};

// Helpers for printing redirections and words (used by the node printers).
void print_redirects(const std::vector<Redirect> &redirs, std::string &out);

}  // namespace gnash::core

#endif  // GNASH_CORE_AST_HPP
