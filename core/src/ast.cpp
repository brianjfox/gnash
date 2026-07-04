// Copyright (c) 2026 Brian J. Fox
// Licensed under GPLv2 with the GPLv2-AI Exception.

// ast.cpp -- canonical rendering of the command AST (analogue of print_cmd.c).
//
// The rendering is a normalized, re-parseable form used for round-trip testing;
// it is not required to be byte-identical to the input, only stable and
// semantically faithful.

#include "gnash/core/ast.hpp"

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

void print_redirects(const std::vector<Redirect> &redirs, std::string &out) {
  for (const Redirect &r : redirs) {
    out += ' ';
    if (r.source_fd >= 0) out += std::to_string(r.source_fd);
    out += op_string(r.op);
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
  out += "(";
  if (body) body->print(out);
  out += ")";
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
    out += "for ((";
    out += a_init;
    out += "; ";
    out += a_cond;
    out += "; ";
    out += a_update;
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

}  // namespace gnash::core
