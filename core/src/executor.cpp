// Copyright (c) 2026 Brian J. Fox
// Licensed under GPLv2 with the GPLv2-AI Exception.

// executor.cpp -- execute the command AST.

#include "gnash/core/executor.hpp"
#include "gnash/core/subscript.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <optional>
#include <termios.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/times.h>
#include <sys/wait.h>

#include "gnash/core/builtins.hpp"
#include "gnash/core/expand.hpp"
#include "gnash/core/lexer.hpp"
#include "strmatch.h"

#if defined(__GLIBC__)
#include <stdio_ext.h>  // __fpurge
#endif

extern "C" char **environ;

namespace gnash::core {

namespace {

struct SavedFd {
  int fd;
  int saved;  // dup of the original, or -1 if the fd was originally closed
};

// Flush a builtin's buffered stdout while its redirections are still active.
// If the write fails -- e.g. stdout was closed with `>&-' or points at a broken
// pipe -- discard the unwritten data so it cannot leak out onto the restored
// descriptor afterward, matching bash (whose builtins write straight to the fd
// and simply lose the output).
void flush_builtin_stdout() {
  if (std::fflush(stdout) == 0) return;
  std::clearerr(stdout);
#if defined(__GLIBC__)
  __fpurge(stdout);
#else
  fpurge(stdout);  // BSD/macOS
#endif
}

// Write BODY to a temp file and return an fd open for reading at offset 0.
int heredoc_fd(const std::string &body) {
  char tmpl[] = "/tmp/gnash_hd_XXXXXX";
  int fd = mkstemp(tmpl);
  if (fd < 0) return -1;
  unlink(tmpl);
  ssize_t off = 0;
  while (off < static_cast<ssize_t>(body.size())) {
    ssize_t w = write(fd, body.data() + off, body.size() - static_cast<size_t>(off));
    if (w <= 0) break;
    off += w;
  }
  lseek(fd, 0, SEEK_SET);
  return fd;
}

void save_fd(int fd, std::vector<SavedFd> &saved) {
  // Backups live well above the fd-10+ region users reach with `exec 10>&1'
  // and the {var} allocations: a user redirect landing on a backup slot would
  // clobber it (`{ exec 10>&1; } > file' must still restore stdout -- the
  // backup used to sit at fd 10 and the inner exec overwrote it, leaving
  // stdout pointing at the file forever).
  int s = fcntl(fd, F_DUPFD_CLOEXEC, 100);
  saved.push_back({fd, s});
}

// Expand a redirection target with the full pipeline (brace, parameter/command/
// arithmetic substitution, word splitting, and pathname expansion) and require
// exactly one resulting word, matching bash's redirection_expand (redir.c): a
// target that expands to zero or more than one word is an `ambiguous redirect'.
// On ambiguity, prints the diagnostic with the ORIGINAL unexpanded word text and
// returns false; otherwise stores the single word in `out'.
static bool expand_redir_target(Shell &sh, const Word &w, std::string &out) {
  Expander ex(sh);
  // POSIX: a non-interactive shell performs no pathname expansion on a
  // redirection target (`cat < redir1.*' opens the literal name).  Suppress
  // globbing for that case without disturbing the other expansions or the word
  // splitting that still makes a multi-word target ambiguous.
  bool saved_noglob = sh.opt_noglob;
  if (sh.opt_posix && !sh.interactive) sh.opt_noglob = true;
  std::vector<std::string> words = ex.expand_args({w});
  sh.opt_noglob = saved_noglob;
  if (words.size() != 1) {
    std::fprintf(stderr, "%s%s: ambiguous redirect\n", sh.err_prefix().c_str(),
                 w.text.c_str());
    return false;
  }
  out = std::move(words[0]);
  return true;
}

// Sentinel from open_redir_output: `set -o noclobber' refused to overwrite an
// existing regular file.
static const int kNoclobberRefused = -2;

// Open an output-redirect target for truncation.  When `set -o noclobber' is in
// effect and this is a clobbering operator (`>' or `&>', not `>|'), refuse to
// overwrite an existing regular file, mirroring bash's noclobber_open (redir.c):
// an existing regular file returns kNoclobberRefused; a missing file is created
// exclusively; a non-regular file (device, fifo, /dev/null) is opened without
// truncation.  Returns the fd, -1 on a system error (errno set), or
// kNoclobberRefused.
static int open_redir_output(Shell &sh, const std::string &fn, bool clobbering) {
  int flags = O_WRONLY | O_CREAT | O_TRUNC;
  if (!(sh.opt_noclobber && clobbering))
    return open(fn.c_str(), flags, 0666);
  struct stat st;
  int r = stat(fn.c_str(), &st);
  if (r == 0 && S_ISREG(st.st_mode)) return kNoclobberRefused;
  flags &= ~O_TRUNC;  // never truncate under noclobber
  if (r != 0) {       // did not exist: create it, failing if someone races us
    int fd = open(fn.c_str(), flags | O_EXCL, 0666);
    return (fd < 0 && errno == EEXIST) ? kNoclobberRefused : fd;
  }
  int fd = open(fn.c_str(), flags, 0666);  // existed, non-regular: append/write
  return (fd < 0 && errno == EEXIST) ? kNoclobberRefused : fd;
}

// Apply one redirect in-process; returns false on error.
bool apply_redirect(Shell &sh, const Redirect &r, std::vector<SavedFd> &saved,
                    const char *fdvar_ctx = nullptr) {
  Expander ex(sh);
  int target_fd = r.source_fd;
  // Install NEWFD as FD, saving FD's previous state, and consume NEWFD.  When
  // the kernel handed us FD itself (open() returns the target number exactly
  // when it was free), the previous state is `closed' -- saving after the
  // open would snapshot the new file, and the callers' close(f) would close
  // the very descriptor just installed (`exec 4>file' with fd 3 occupied).
  auto redir_to = [&](int newfd, int fd) {
    if (newfd == fd) {
      saved.push_back({fd, -1});
      return;
    }
    save_fd(fd, saved);
    dup2(newfd, fd);
    close(newfd);
  };
  // `&>file' / bare `>&file': install F as BOTH stdout and stderr, consuming
  // it, with the same handed-us-the-target-fd care as redir_to.
  auto install_both = [&](int f) {
    for (int t : {1, 2}) {
      if (f == t) {
        saved.push_back({t, -1});
        continue;
      }
      save_fd(t, saved);
      dup2(f, t);
    }
    if (f != 1 && f != 2) close(f);
  };

  // A restricted shell forbids output redirections (creating/truncating/
  // appending files, and fd duplication that writes).
  if (sh.opt_restricted) {
    switch (r.op) {
      case RedirOp::OutputRedir:
      case RedirOp::Clobber:
      case RedirOp::AppendOutput:
      case RedirOp::InputOutput:
      case RedirOp::AndOutput:
      case RedirOp::AndAppend: {
        Expander rex(sh);
        std::string fn = rex.expand_no_split(r.target.text, true);
        std::fprintf(stderr, "%s%s: restricted: cannot redirect output\n",
                     sh.err_prefix().c_str(), fn.c_str());
        return false;
      }
      default: break;
    }
  }

  // `{var}<file' etc.: allocate a fresh high, close-on-exec descriptor, open the
  // redirection on it, and store its number in the variable.  The descriptor is
  // recorded as an originally-closed fd so a normal command closes it afterwards
  // while `exec' (discard_saved_fds) keeps it open.  `{var}>&-' / `{var}<&-'
  // instead close the descriptor whose number the variable currently holds.
  if (!r.fd_var.empty()) {
    // Store the fd number into the {var} target, which may be an array
    // element (`exec {fd[0]}<&0').  A readonly (or otherwise unassignable)
    // target fails the whole redirection with bash's diagnostic.
    auto store_var = [&](int f) -> bool {
      size_t br = r.fd_var.find('[');
      bool ok;
      if (br != std::string::npos) {
        std::string base = r.fd_var.substr(0, br);
        std::string sub = r.fd_var.substr(br + 1, r.fd_var.size() - br - 2);
        auto it = sh.vars.find(sh.deref(base));
        ok = !(it != sh.vars.end() && it->second.readonly);
        if (ok) sh.array_set(base, sub, std::to_string(f));
      } else {
        ok = sh.set(r.fd_var, std::to_string(f), fdvar_ctx);
      }
      if (!ok) {
        std::fprintf(stderr, "%s%s: cannot assign fd to variable\n", sh.err_prefix().c_str(),
                     r.fd_var.c_str());
        close(f);
        return false;
      }
      // bash leaves a {var} descriptor OPEN after the command completes; only
      // with `shopt -s varredir_close' is it closed like other redirections.
      auto vit = sh.shopt_opts.find("varredir_close");
      if (vit != sh.shopt_opts.end() && vit->second) saved.push_back({f, -1});
      return true;
    };
    // Read the fd number the {var} target currently holds (for the close and
    // move forms).
    auto read_var = [&]() -> std::string {
      size_t br = r.fd_var.find('[');
      if (br != std::string::npos)
        return sh.array_get(r.fd_var.substr(0, br),
                            r.fd_var.substr(br + 1, r.fd_var.size() - br - 2));
      return sh.get(r.fd_var);
    };
    auto assign_fd = [&](int f, const std::string &fn = std::string()) {
      int hi = fcntl(f, F_DUPFD_CLOEXEC, 10);
      if (hi < 0) {
        // The move to a high descriptor failed (e.g. `ulimit -n' below 10):
        // bash reports the duplication failure (no line number) and then the
        // target with the same errno, and the redirection fails.
        int e = errno;
        std::fprintf(stderr, "%s: redirection error: cannot duplicate fd: %s\n",
                     sh.shell_name.c_str(), std::strerror(e));
        if (!fn.empty())
          std::fprintf(stderr, "%s%s: %s\n", sh.err_prefix().c_str(), fn.c_str(),
                       std::strerror(e));
        close(f);
        return false;
      }
      close(f);
      return store_var(hi);
    };
    auto open_var = [&](int flags) -> bool {
      std::string fn;
      if (!expand_redir_target(sh, r.target, fn)) return false;
      int f = open(fn.c_str(), flags, 0666);
      if (f < 0) {
        std::fprintf(stderr, "%s%s: %s\n", sh.err_prefix().c_str(), fn.c_str(),
                     std::strerror(errno));
        return false;
      }
      return assign_fd(f, fn);
    };
    switch (r.op) {
      case RedirOp::InputRedir:   return open_var(O_RDONLY);
      case RedirOp::OutputRedir:
      case RedirOp::Clobber:      return open_var(O_WRONLY | O_CREAT | O_TRUNC);
      case RedirOp::AppendOutput: return open_var(O_WRONLY | O_CREAT | O_APPEND);
      case RedirOp::InputOutput:  return open_var(O_RDWR | O_CREAT);
      case RedirOp::DupOutput:
      case RedirOp::DupInput: {
        std::string w = ex.expand_no_split(r.target.text);
        if (w == "-") {  // close the descriptor the variable names
          std::string cur = read_var();
          if (cur.empty()) {
            // `exec {v}>&-' with no fd number in the variable: bash reports
            // the bare variable name as an ambiguous redirect.
            std::fprintf(stderr, "%s%s: ambiguous redirect\n", sh.err_prefix().c_str(),
                         r.fd_var.c_str());
            return false;
          }
          int n = std::atoi(cur.c_str());
          if (n >= 0) close(n);
          return true;
        }
        // `{v}<&N-' moves: duplicate N then close it.
        bool move = w.size() > 1 && w.back() == '-';
        if (move) w.pop_back();
        int src = std::atoi(w.c_str());
        int f = fcntl(src, F_DUPFD_CLOEXEC, 10);
        if (f < 0) {
          // The diagnostic names the UNEXPANDED word (`$fd: Bad file
          // descriptor'), as bash does.
          std::fprintf(stderr, "%s%s: %s\n", sh.err_prefix().c_str(), r.target.text.c_str(),
                       std::strerror(errno));
          return false;
        }
        if (move) close(src);
        return store_var(f);
      }
      case RedirOp::HereDoc:
      case RedirOp::HereDocStrip: {
        bool saved_ae = sh.arith_error;
        sh.arith_error = false;
        std::string body = r.heredoc_quoted ? r.heredoc_body : ex.expand_heredoc(r.heredoc_body);
        // An expansion failure in the body (`${'x1'%'t'}: bad substitution')
        // aborts the redirection -- and with it the command -- as bash does
        // (posixexp7.sub); the diagnostic was already printed, and later
        // commands are unaffected.
        if (sh.arith_error) {
          sh.arith_error = saved_ae;
          return false;
        }
        sh.arith_error = saved_ae;
        int f = heredoc_fd(body);
        if (f < 0) return false;
        return assign_fd(f);
      }
      case RedirOp::HereString: {
        std::string body = ex.expand_no_split(r.target.text) + "\n";
        int f = heredoc_fd(body);
        if (f < 0) return false;
        return assign_fd(f);
      }
      default: break;
    }
    return true;
  }

  switch (r.op) {
    case RedirOp::InputRedir: {
      std::string fn;
      if (!expand_redir_target(sh, r.target, fn)) return false;
      int f = open(fn.c_str(), O_RDONLY);
      if (f < 0) { std::fprintf(stderr, "%s%s: %s\n", sh.err_prefix().c_str(), fn.c_str(), std::strerror(errno)); return false; }
      redir_to(f, target_fd < 0 ? 0 : target_fd);
      return true;
    }
    case RedirOp::OutputRedir:
    case RedirOp::Clobber: {
      std::string fn;
      if (!expand_redir_target(sh, r.target, fn)) return false;
      // `>' honors noclobber; `>|' (Clobber) always overrides it.
      int f = open_redir_output(sh, fn, r.op == RedirOp::OutputRedir);
      if (f == kNoclobberRefused) {
        std::fprintf(stderr, "%s%s: cannot overwrite existing file\n",
                     sh.err_prefix().c_str(), fn.c_str());
        return false;
      }
      if (f < 0) { std::fprintf(stderr, "%s%s: %s\n", sh.err_prefix().c_str(), fn.c_str(), std::strerror(errno)); return false; }
      redir_to(f, target_fd < 0 ? 1 : target_fd);
      return true;
    }
    case RedirOp::AppendOutput: {
      std::string fn;
      if (!expand_redir_target(sh, r.target, fn)) return false;
      int f = open(fn.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0666);
      if (f < 0) { std::fprintf(stderr, "%s%s: %s\n", sh.err_prefix().c_str(), fn.c_str(), std::strerror(errno)); return false; }
      redir_to(f, target_fd < 0 ? 1 : target_fd);
      return true;
    }
    case RedirOp::InputOutput: {
      std::string fn;
      if (!expand_redir_target(sh, r.target, fn)) return false;
      int f = open(fn.c_str(), O_RDWR | O_CREAT, 0666);
      if (f < 0) return false;
      redir_to(f, target_fd < 0 ? 0 : target_fd);
      return true;
    }
    case RedirOp::AndOutput:
    case RedirOp::AndAppend: {
      std::string fn;
      if (!expand_redir_target(sh, r.target, fn)) return false;
      int f;
      if (r.op == RedirOp::AndAppend) {
        f = open(fn.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0666);
      } else {  // `&>' truncates, so it honors noclobber
        f = open_redir_output(sh, fn, true);
        if (f == kNoclobberRefused) {
          std::fprintf(stderr, "%s%s: cannot overwrite existing file\n",
                       sh.err_prefix().c_str(), fn.c_str());
          return false;
        }
      }
      if (f < 0) return false;
      install_both(f);
      return true;
    }
    case RedirOp::DupOutput:
    case RedirOp::DupInput: {
      // `<&word' / `>&word' likewise reject a word that splits to zero or more
      // than one field (an unset or multi-word fd, e.g. `<&$unset').
      std::string w;
      if (!expand_redir_target(sh, r.target, w)) return false;
      int deffd = (r.op == RedirOp::DupInput) ? 0 : 1;
      int fd = target_fd < 0 ? deffd : target_fd;
      if (w == "-") { save_fd(fd, saved); close(fd); return true; }
      // The source must be a plain fd number (optionally with a trailing `-'
      // for the move form).  Anything else -- a negative or non-numeric value
      // such as `<&$fd' with fd=-1 -- is an ambiguous redirect, as in bash.
      bool valid_fd = !w.empty();
      for (size_t k = 0; k < w.size() && valid_fd; k++) {
        if (std::isdigit(static_cast<unsigned char>(w[k]))) continue;
        if (w[k] == '-' && k + 1 == w.size() && k > 0) continue;  // trailing move `-'
        valid_fd = false;
      }
      if (!valid_fd) {
        // A bare `>&word' (no explicit source fd) whose word is a filename
        // rather than an fd is bash's shorthand for redirecting BOTH stdout and
        // stderr to that file, equivalent to `&>word'.  With an explicit source
        // fd (`2>&word') or for input (`<&word') a non-fd word is ambiguous.
        if (r.op == RedirOp::DupOutput && target_fd < 0 && !w.empty()) {
          int f = open_redir_output(sh, w, true);  // `>&file' honors noclobber
          if (f == kNoclobberRefused) {
            std::fprintf(stderr, "%s%s: cannot overwrite existing file\n",
                         sh.err_prefix().c_str(), w.c_str());
            return false;
          }
          if (f < 0) {
            std::fprintf(stderr, "%s%s: %s\n", sh.err_prefix().c_str(), w.c_str(),
                         std::strerror(errno));
            return false;
          }
          install_both(f);
          return true;
        }
        std::fprintf(stderr, "%s%s: ambiguous redirect\n", sh.err_prefix().c_str(),
                     w.c_str());
        return false;
      }
      int src = std::atoi(w.c_str());
      // A source fd that is not open is an error, reported with the
      // UNEXPANDED word (`echo foo >&$fd' -> `$fd: Bad file descriptor').
      if (fcntl(src, F_GETFD) < 0) {
        std::fprintf(stderr, "%s%s: %s\n", sh.err_prefix().c_str(), r.target.text.c_str(),
                     std::strerror(EBADF));
        return false;
      }
      save_fd(fd, saved);
      dup2(src, fd);
      if (w.back() == '-') close(src);  // the move form `[n]<&digit-'
      return true;
    }
    case RedirOp::HereDoc:
    case RedirOp::HereDocStrip: {
      bool saved_ae = sh.arith_error;
      sh.arith_error = false;
      std::string body = r.heredoc_quoted ? r.heredoc_body : ex.expand_heredoc(r.heredoc_body);
      // A bad substitution in the body aborts the redirection (bash).
      if (sh.arith_error) {
        sh.arith_error = saved_ae;
        return false;
      }
      sh.arith_error = saved_ae;
      int f = heredoc_fd(body);
      if (f < 0) return false;
      redir_to(f, target_fd < 0 ? 0 : target_fd);
      return true;
    }
    case RedirOp::HereString: {
      std::string body = ex.expand_no_split(r.target.text) + "\n";
      int f = heredoc_fd(body);
      if (f < 0) return false;
      redir_to(f, target_fd < 0 ? 0 : target_fd);
      return true;
    }
  }
  return true;
}

bool apply_redirects(Shell &sh, const std::vector<Redirect> &redirs, std::vector<SavedFd> &saved,
                     const char *fdvar_ctx = nullptr) {
  for (const Redirect &r : redirs)
    if (!apply_redirect(sh, r, saved, fdvar_ctx)) return false;
  return true;
}

void restore_fds(std::vector<SavedFd> &saved) {
  for (auto it = saved.rbegin(); it != saved.rend(); ++it) {
    if (it->saved >= 0) {
      dup2(it->saved, it->fd);
      close(it->saved);
    } else {
      close(it->fd);
    }
  }
  saved.clear();
}

// Make the applied redirections permanent (`exec < file'): drop the backups
// instead of restoring from them.
void discard_saved_fds(std::vector<SavedFd> &saved) {
  for (const SavedFd &s : saved)
    if (s.saved >= 0) close(s.saved);
  saved.clear();
}

// ---- assignments (scalar, array element, and array literal) --------------

struct Assign {
  std::string name;
  std::optional<std::string> sub;  // subscript, if name[sub]=
  bool append = false;             // +=
  bool is_array = false;           // value is (...)
  std::string value;
};

bool parse_assign(const std::string &w, Assign &a) {
  std::string sub;
  bool has_sub = false;
  size_t i = split_subscript(w, a.name, sub, has_sub);
  if (i == std::string::npos) return false;  // not a name-headed word
  if (has_sub) a.sub = sub;
  if (i < w.size() && w[i] == '+') { a.append = true; i++; }
  if (i >= w.size() || w[i] != '=') return false;
  i++;
  a.value = w.substr(i);
  a.is_array = a.value.size() >= 2 && a.value.front() == '(' && a.value.back() == ')';
  return true;
}

}  // namespace (parse_array_elems is shared with bi_declare)

std::vector<std::pair<std::optional<std::string>, std::string>>
parse_array_elems(Shell &sh, Expander &ex, const std::string &name, bool integer,
                  bool whole_append, const std::string &parenval) {
  std::vector<std::pair<std::optional<std::string>, std::string>> out;
  std::string inner = parenval.substr(1, parenval.size() - 2);
  // For an indexed array, validate explicit subscripts and resolve negatives
  // against the highest index seen so far (bash processes the list left to
  // right); an associative array takes any string as a key.
  auto tvit = sh.vars.find(sh.deref(name));
  bool assoc = tvit != sh.vars.end() && tvit->second.kind == VarKind::Assoc;
  // A fresh `name=(...)' has already cleared the array, so negative subscripts
  // resolve against the running max starting at -1.  An append `name+=(...)'
  // keeps the existing elements, so a leading `[-1]=v' counts back from the
  // current highest index (bash).
  long long maxidx = -1;
  if (whole_append && !assoc && tvit != sh.vars.end() && !tvit->second.idx.empty())
    maxidx = tvit->second.idx.rbegin()->first;
  // An associative array whose list already used `[key]=value' form is in
  // subscript mode: a later bare word is an error.  An all-bare list is a valid
  // flat key/value list, so only reject a bare word once a subscript was seen.
  bool saw_sub = false;
  // The general tokenizer word-splits an unquoted subscript that contains a
  // space ([a b]=v -> "[a", "b]=v"); reassemble any element whose leading `['
  // subscript is not yet closed so the [sub]=value parse below sees the whole
  // key.  (A quoted subscript ["a b"] already survives as a single token.)
  // Runs of whitespace inside the subscript collapse to one space -- quote the
  // key to preserve exact spacing, as in bash.
  std::vector<Token> toks = tokenize(inner);
  std::vector<Token> elems;
  for (size_t k = 0; k < toks.size(); k++) {
    Token t = toks[k];
    while (t.type == Tok::Word && !t.text.empty() && t.text[0] == '[' &&
           skip_subscript(t.text, 0) == std::string::npos &&
           k + 1 < toks.size() && toks[k + 1].type == Tok::Word) {
      t.text += ' ' + toks[k + 1].text;
      t.quoted = t.quoted || toks[k + 1].quoted;
      k++;
    }
    elems.push_back(t);
  }
  for (const Token &t : elems) {
    if (t.type == Tok::Eof) break;
    if (t.type != Tok::Word) continue;
    const std::string &e = t.text;
    if (!e.empty() && e[0] == '[') {
      size_t rb = skip_subscript(e, 0);  // the ']' closing the [subscript]
      // [sub]=value or [sub]+=value (append to that element).
      bool app = rb != std::string::npos && rb + 2 < e.size() && e[rb + 1] == '+' &&
                 e[rb + 2] == '=';
      bool plain = rb != std::string::npos && rb + 1 < e.size() && e[rb + 1] == '=';
      if (app || plain) {
        std::string sub = ex.expand_no_split(e.substr(1, rb - 1));
        // Bug-compat: in the ARITHMETIC (indexed) path a \001 byte in the
        // expanded subscript collides with bash's internal CTLESC and
        // vanishes (quoting the byte after it), so `[$'x\001y\177z']=foo'
        // errors on `xy^?z', not `x^Ay^?z'.  An ASSOCIATIVE key keeps its
        // bytes intact (exp8.sub tests both).
        if (!assoc)
          for (size_t cb = 0; cb < sub.size();)
            if (sub[cb] == '\001') sub.erase(cb, 1), cb++;
            else cb++;
        // The value of a `[sub]=value' element is an assignment RHS, so a `~'
        // tilde-expands at the start and after each unquoted `:' (bash); a bare
        // word element, handled below, only gets leading-tilde expansion.
        std::string val = ex.expand_assignment(e.substr(rb + (app ? 3 : 2)));
        if (!assoc) {
          // bash stops the whole compound assignment at the first bad subscript
          // (the array has already been cleared, so the partial result stands).
          if (sub.empty()) {
            std::fprintf(stderr, "%s%s: bad array subscript\n",
                         sh.err_prefix().c_str(), e.c_str());
            break;
          }
          if (sub == "*" || sub == "@") {
            std::fprintf(stderr, "%s%s: cannot assign to non-numeric index\n",
                         sh.err_prefix().c_str(), e.c_str());
            break;
          }
          // `shopt -s array_expand_once': a compound assignment must not perform
          // a second expansion of the subscript (an injection attempt errors).
          if (!sh.array_expand_once_ok(name, sub)) break;  // diagnostic printed
          bool ok = true;
          long long k = eval_arith(sh, sub, &ok);
          if (!ok) {
            // Report with bash's subscript diagnostic and abort the whole
            // assignment (the array keeps whatever was assigned so far).
            eval_arith_msg(sh, sub, "", &ok);
            out.clear();
            break;
          }
          ok = true;
          if (ok && k < 0) k += maxidx + 1;  // resolve negative against running max
          if (ok && k < 0) {
            std::fprintf(stderr, "%s%s: bad array subscript\n",
                         sh.err_prefix().c_str(), e.c_str());
            break;
          }
          if (ok) { sub = std::to_string(k); if (k > maxidx) maxidx = k; }
        }
        if (app) {  // resolve against the current element (fresh assign cleared
                    // it, so the base is 0 unless this is a whole-array append)
          std::string base = whole_append ? sh.array_get(name, sh.zsh_subscript(name, sub)) : "0";
          if (integer) {
            bool ok = true;
            val = std::to_string(eval_arith(sh, base, &ok) + eval_arith(sh, val, &ok));
          } else {
            val = base + val;
          }
        }
        out.emplace_back(sub, val);
        saw_sub = true;
        continue;
      }
    }
    // An associative array in subscript mode cannot take a bare value: bash
    // reports the offending word and stops the assignment.
    if (assoc && saw_sub) {
      std::fprintf(stderr,
                   "%s%s: %s: must use subscript when assigning associative array\n",
                   sh.err_prefix().c_str(), name.c_str(), e.c_str());
      break;
    }
    if (assoc) {
      // An associative key/value list expands each word WITHOUT word
      // splitting or pathname expansion (bash assign_assoc_from_kvlist):
      // `v1=( $foo 3 )' with foo='1 2' keys on "1 2" whole, and an unquoted
      // `*' stays a literal key (assoc11.sub, assoc12.sub).
      out.emplace_back(std::nullopt, ex.expand_no_split(e, false, true));
      continue;
    }
    for (const std::string &f : ex.expand_args({Word{e, t.quoted ? W_QUOTED : 0}})) {
      out.emplace_back(std::nullopt, f);
      maxidx++;  // a positional element lands at the next index
    }
  }
  return out;
}

namespace {

void apply_array_assign(Shell &sh, Expander &ex, const Assign &a,
                        const char *ctx = nullptr) {
  // Assigning to any part of a readonly array is an error (bash names the array,
  // not the element).  For an ELEMENT assignment the subscript validates
  // FIRST: `readonly -a c; c[-2]=4' is the bad-subscript error, so let
  // array_set order the checks; only the compound form rejects here.
  {
    auto rit = sh.vars.find(sh.deref(a.name));
    if (!a.sub && rit != sh.vars.end() && rit->second.readonly) {
      const std::string &fn =
          sh.call_stack.empty() ? std::string() : sh.call_stack.back().func;
      std::string who = ctx ? std::string(ctx) : fn;
      std::fprintf(stderr, "%s%s%s%s: readonly variable\n", sh.err_prefix().c_str(),
                   who.c_str(), who.empty() ? "" : ": ", a.name.c_str());
      sh.last_status = 1;
      return;
    }
  }
  // A nameref whose target is itself an array element (`declare -n ref=a[0]')
  // cannot be subscripted further -- `ref[foo]=x' would mean `a[0][foo]'.  bash
  // rejects the resolved target as an invalid identifier rather than creating a
  // variable literally named `a[0]'.
  if (a.sub) {
    // A targetless nameref (`declare -n ref') resolves to the empty name, so
    // `ref[0]=x' has nothing to subscript: bash rejects it as `': not a valid
    // identifier' and leaves ref an unset reference.
    auto nit = sh.vars.find(a.name);
    if (nit != sh.vars.end() && nit->second.nameref && nit->second.value.empty()) {
      std::fprintf(stderr, "%s`': not a valid identifier\n", sh.err_prefix().c_str());
      sh.last_status = 1;
      return;
    }
    std::string dn = sh.deref(a.name);
    if (dn.find('[') != std::string::npos) {
      std::fprintf(stderr, "%s`%s': not a valid identifier\n", sh.err_prefix().c_str(),
                   dn.c_str());
      sh.last_status = 1;
      return;
    }
  }
  // An empty subscript (`b[]=x') is always a bad array subscript.  The special
  // `*'/`@' selectors are bad for an indexed array but are ordinary literal keys
  // for an associative one (`a[@]=x' stores under the key "@").
  if (a.sub) {
    auto sit = sh.vars.find(sh.deref(a.name));
    bool assoc = sit != sh.vars.end() && sit->second.kind == VarKind::Assoc;
    if (a.sub->empty() || (!assoc && (*a.sub == "*" || *a.sub == "@"))) {
      std::fprintf(stderr, "%s%s[%s]: bad array subscript\n", sh.err_prefix().c_str(),
                   a.name.c_str(), a.sub->c_str());
      sh.last_status = 1;
      return;
    }
  }
  // An integer-attributed array (`declare -i') evaluates each element value as
  // an arithmetic expression, and `+=' adds rather than string-appends.  The
  // attribute lives on the target the assignment lands on, so resolve through a
  // nameref (`declare -ai var; declare -n ref=var; ref[1]=' evaluates on var).
  std::string dtgt = sh.deref(a.name);
  size_t dlb = dtgt.find('[');
  auto vit = sh.vars.find(dlb == std::string::npos ? dtgt : dtgt.substr(0, dlb));
  bool integer = vit != sh.vars.end() && vit->second.integer;
  if (a.sub) {
    // A compound value `(...)' cannot be assigned to a single element in bash
    // (`a[0]=(x y)').  zsh has its own array semantics, so only enforce this
    // outside the zsh personality.
    if (a.is_array && !sh.is_zsh()) {
      std::fprintf(stderr, "%s%s[%s]: cannot assign list to array member\n",
                   sh.err_prefix().c_str(), a.name.c_str(), a.sub->c_str());
      sh.last_status = 1;
      return;
    }
    // zsh array subscripts are 1-based; translate to the internal 0-based index
    // (a no-op under other personalities / for associative arrays).
    std::string sub = ex.expand_no_split(*a.sub);
    // `shopt -s array_expand_once': reject an un-evaluatable (e.g. injected)
    // subscript rather than silently using index 0.
    if (!sh.array_expand_once_ok(a.name, sub)) { sh.arith_error = true; return; }
    sub = sh.zsh_subscript(a.name, sub);
    std::string val = ex.expand_assignment(a.value);
    if (integer) {
      bool ok = true;
      long long rhs = eval_arith(sh, val, &ok);
      long long base = a.append ? eval_arith(sh, sh.array_get(a.name, sub), &ok) : 0;
      val = std::to_string(base + rhs);
    } else if (a.append) {
      val = sh.array_get(a.name, sub) + val;
    }
    sh.array_set(a.name, sub, val);
  } else {  // is_array
    bool assoc = sh.vars.count(a.name) && sh.vars[a.name].kind == VarKind::Assoc;
    auto elems = parse_array_elems(sh, ex, a.name, integer, a.append, a.value);
    if (integer)
      for (auto &e : elems) {
        bool ok = true;
        e.second = std::to_string(eval_arith(sh, e.second, &ok));
      }
    sh.array_assign(a.name, elems, a.append, assoc);
  }
}

void gather_pipeline(const Command *c, std::vector<const Command *> &stages) {
  const auto *conn = dynamic_cast<const Connection *>(c);
  if (conn && conn->conn == Connector::Pipe) {
    gather_pipeline(conn->first.get(), stages);
    stages.push_back(conn->second.get());
  } else {
    stages.push_back(c);
  }
}

}  // namespace

namespace {
// A NAME= / NAME+= / NAME[sub]= prefix (an assignment word for a builtin).
bool is_assignment_word_text(const std::string &w) {
  size_t i = 0;
  if (i >= w.size() || !(std::isalpha(static_cast<unsigned char>(w[i])) || w[i] == '_'))
    return false;
  while (i < w.size() && (std::isalnum(static_cast<unsigned char>(w[i])) || w[i] == '_')) i++;
  if (i < w.size() && w[i] == '[') {
    size_t close = skip_subscript(w, i);
    if (close == std::string::npos) return false;
    i = close + 1;
  }
  if (i < w.size() && w[i] == '+') i++;
  return i < w.size() && w[i] == '=';
}

// Is WORD a compound assignment as WRITTEN -- `NAME=(' with the `=' and the
// `(' both outside quoting?  See Shell::RawArg::compound_assign.
bool is_compound_assignment_source(const std::string &w) {
  char q = 0;
  for (size_t i = 0; i < w.size(); i++) {
    char c = w[i];
    if (q) { if (c == q) q = 0; else if (c == '\\' && q == '"') i++; continue; }
    if (c == '\'' || c == '"') { q = c; continue; }
    if (c == '\\') { i++; continue; }
    if (c == '=') return i + 1 < w.size() && w[i + 1] == '(';
  }
  return false;
}

bool is_assignment_builtin(const std::string &cmd) {
  return cmd == "declare" || cmd == "typeset" || cmd == "local" ||
         cmd == "readonly" || cmd == "export";
}
}  // namespace

void apply_assignment_word(Shell &sh, const std::string &word, const char *ctx) {
  Expander ex(sh);
  Assign a;
  if (!parse_assign(word, a)) return;
  if (a.sub || a.is_array)
    apply_array_assign(sh, ex, a, ctx);
  else
    sh.set(a.name, ex.expand_assignment(a.value));
}

// Format one %-directive value for `time' (bash's TIMEFORMAT).  A plain form
// prints total seconds with PREC decimals; the long form ("l") prints
// MINUTESmSECONDSs.
static std::string time_value(double sec, int prec, bool longfmt) {
  if (sec < 0) sec = 0;
  char buf[64];
  if (longfmt) {
    long m = static_cast<long>(sec / 60);
    double s = sec - static_cast<double>(m) * 60.0;
    std::snprintf(buf, sizeof buf, "%ldm%.*fs", m, prec, s);
  } else {
    std::snprintf(buf, sizeof buf, "%.*f", prec, sec);
  }
  return buf;
}

// Render a `time' report from the elapsed real/user/sys seconds, following
// bash's TIMEFORMAT grammar (%[p][l]{R|U|S|P}, %%); `time -p' forces the POSIX
// format.  A trailing newline terminates the report.
static std::string time_report(Shell &sh, bool posix, double real, double user,
                               double sys) {
  std::string fmt;
  if (posix) {
    fmt = "real %2R\nuser %2U\nsys %2S";
  } else if (!sh.get_if_set("TIMEFORMAT", fmt)) {
    fmt = "\nreal\t%3lR\nuser\t%3lU\nsys\t%3lS";
  }
  std::string out;
  for (size_t i = 0; i < fmt.size(); i++) {
    if (fmt[i] != '%') { out += fmt[i]; continue; }
    if (++i >= fmt.size()) { out += '%'; break; }
    if (fmt[i] == '%') { out += '%'; continue; }
    int prec = 3;
    if (std::isdigit(static_cast<unsigned char>(fmt[i]))) { prec = fmt[i] - '0'; i++; }
    bool longfmt = false;
    if (i < fmt.size() && fmt[i] == 'l') { longfmt = true; i++; }
    if (i >= fmt.size()) break;
    switch (fmt[i]) {
      case 'R': out += time_value(real, prec, longfmt); break;
      case 'U': out += time_value(user, prec, longfmt); break;
      case 'S': out += time_value(sys, prec, longfmt); break;
      case 'P': {  // %CPU = (user+sys)/real, never long form
        double p = real > 0 ? (user + sys) / real * 100.0 : 0.0;
        out += time_value(p, prec, false);
        break;
      }
      default: out += '%'; out += fmt[i]; break;
    }
  }
  out += '\n';
  return out;
}

int Executor::run(const Command *c) {
  if (!c || unwinding()) return sh_.last_status;

  // `time PIPELINE': measure and report real/user/sys time.  Re-enter run() with
  // the flag suppressed for this node so the body runs exactly once.
  if ((c->flags & CMD_TIME) && c != timed_cmd_) {
    const Command *prev = timed_cmd_;
    timed_cmd_ = c;
    struct timeval w0, w1;
    struct tms t0, t1;
    gettimeofday(&w0, nullptr);
    times(&t0);
    int st = run(c);
    times(&t1);
    gettimeofday(&w1, nullptr);
    timed_cmd_ = prev;
    long hz = sysconf(_SC_CLK_TCK);
    if (hz <= 0) hz = 100;
    double real = (w1.tv_sec - w0.tv_sec) + (w1.tv_usec - w0.tv_usec) / 1e6;
    double user = static_cast<double>((t1.tms_utime - t0.tms_utime) +
                                      (t1.tms_cutime - t0.tms_cutime)) / hz;
    double sys = static_cast<double>((t1.tms_stime - t0.tms_stime) +
                                     (t1.tms_cstime - t0.tms_cstime)) / hz;
    std::string rep = time_report(sh_, (c->flags & CMD_TIME_POSIX) != 0, real, user, sys);
    std::fputs(rep.c_str(), stderr);
    return st;
  }

  // `-n' (noexec): parse commands but never run them, so a script can be
  // syntax-checked without side effects.  Gating each command here -- rather
  // than skipping the whole tree in run_string -- matches bash: a `set -n'
  // reached mid-script executes (turning noexec on), after which every later
  // command is skipped, and a subsequent `set +n' never runs to turn it back
  // off.  Ignored by interactive shells, exactly as bash does.
  if (sh_.opt_noexec) return sh_.last_status;

  sh_.run_pending_traps();  // deliver any signals received between commands

  // A dead coprocess is torn down (fds closed, NAME/NAME_PID unset) before
  // the next command runs, as bash's SIGCHLD-driven coproc_reap does.
  if (sh_.coproc_pid) sh_.reap_coproc();

  // $LINENO / error line.  bash installs a command's line for only a FEW
  // command types -- SET_LINE_NUMBER() appears in execute_cmd.c for the simple
  // command (run_simple sets its own, below), the subshell, `(( ))' and
  // `[[ ]]', and `for'/`select' set it directly.  `while', `until', `if',
  // `case', a `{ }' group and a function definition deliberately do NOT: they
  // leave line_number wherever it already was, which is why a diagnostic from
  // inside e.g. `(...)' reports the subshell's closing line rather than the
  // line of the command that produced it.  Matching that set matters --
  // updating it for every compound made redirection errors inside a subshell
  // report the wrong line all through redir12.sub.
  // ...and it is SCOPED: bash saves line_number, installs the command's, and
  // restores it when the command finishes (the "simple_lineno" unwind frame).
  // So the line in force is always the innermost enclosing construct that set
  // one, not whatever the last command to run happened to leave behind.
  struct LineGuard {
    Shell &sh; int saved;
    explicit LineGuard(Shell &s) : sh(s), saved(s.cur_lineno) {}
    ~LineGuard() { sh.cur_lineno = saved; }
  } lg(sh_);
  // A subshell installs its line BEFORE its redirections, which bash applies in
  // the forked child; the others install theirs only once redirections have
  // succeeded, so a redirection error on `for ... done > f' inside a subshell
  // still reports the subshell's line rather than the `for'.
  if (c->line > 0 && dynamic_cast<const Subshell *>(c))
    sh_.cur_lineno = sh_.lineno_base + c->line;

  // A negated command (`! cmd') never triggers errexit, and neither do the
  // commands nested within it -- bash exempts the entire subtree, so that e.g.
  // the `false' inside `! eval false' does not exit a `set -e' shell.  Suppress
  // errexit for the duration of the negated command's execution.
  struct ErrexitGuard {
    Shell &sh; bool active;
    ErrexitGuard(Shell &s, bool a) : sh(s), active(a) { if (active) sh.errexit_suppress++; }
    ~ErrexitGuard() { if (active) sh.errexit_suppress--; }
  } eg(sh_, (c->flags & CMD_INVERT_RETURN) != 0);

  if (auto *p = dynamic_cast<const SimpleCommand *>(c)) return run_simple(p);
  if (auto *p = dynamic_cast<const Connection *>(c)) return run_connection(p);

  // Compound commands: apply redirects in-process around the body.  A
  // redirection failure runs the ERR trap and is fatal under `set -e',
  // exactly like a failing simple command (redir12.sub).
  std::vector<SavedFd> saved;
  if (!apply_redirects(sh_, c->redirects, saved)) {
    restore_fds(saved);
    sh_.last_status = 1;
    if (sh_.errexit_suppress == 0 && !unwinding()) {
      sh_.run_err_trap(1);
      if (sh_.opt_errexit) {
        sh_.exiting = true;
        sh_.exit_status = 1;
      }
    }
    return sh_.last_status;
  }
  // (`select' installs its line INSIDE run_select, after the loop-variable
  // identifier check: bash's execute_select_command calls check_identifier
  // before `line_number = select_command->line', so that diagnostic reports
  // the ambient line -- errors.tests `bad-select'.)
  {
    auto *fc = dynamic_cast<const ForCommand *>(c);
    if (c->line > 0 && (dynamic_cast<const ArithCommand *>(c) ||
                        dynamic_cast<const CondCommand *>(c) ||
                        (fc && !fc->is_select)))
      sh_.cur_lineno = sh_.lineno_base + c->line;
  }
  int st = sh_.last_status;
  if (auto *pa = dynamic_cast<const Subshell *>(c)) st = run_subshell(pa);
  else if (auto *pb = dynamic_cast<const Group *>(c)) st = run_group(pb);
  else if (auto *pc = dynamic_cast<const IfCommand *>(c)) st = run_if(pc);
  else if (auto *pd = dynamic_cast<const LoopCommand *>(c)) st = run_loop(pd);
  else if (auto *pe = dynamic_cast<const ForCommand *>(c)) st = run_for(pe);
  else if (auto *pf = dynamic_cast<const CaseCommand *>(c)) st = run_case(pf);
  else if (auto *pg = dynamic_cast<const FunctionDef *>(c)) st = run_funcdef(pg);
  else if (auto *ph = dynamic_cast<const CondCommand *>(c)) st = run_cond(ph);
  else if (auto *pi = dynamic_cast<const ArithCommand *>(c)) st = run_arith(pi);
  else if (auto *pj = dynamic_cast<const CoprocCommand *>(c)) st = run_coproc(pj);
  restore_fds(saved);
  if (c->flags & CMD_INVERT_RETURN) st = st ? 0 : 1;
  sh_.last_status = st;
  // A compound command (subshell, group, if, loop, ...) that returns non-zero
  // triggers errexit at its own level, e.g. `set -e; (exit 17)' exits the shell.
  // Conditions and negations are already exempted via errexit_suppress / the
  // CMD_INVERT_RETURN guard in run().  The ERR trap fires for a subshell (a
  // process boundary) but not for a transparent compound wrapper -- a group,
  // if/while/for, or function body -- whose own commands already fired it.
  if (st != 0 && sh_.errexit_suppress == 0 && !unwinding() &&
      !(c->flags & CMD_INVERT_RETURN)) {
    // A subshell is a process boundary; `[[ ]]' and `(( ))' are leaves that
    // failed on their own account.  The transparent wrappers -- group, if,
    // loop, function body -- stay silent, since the commands inside them
    // already fired it.
    if (dynamic_cast<const Subshell *>(c) || dynamic_cast<const CondCommand *>(c) ||
        dynamic_cast<const ArithCommand *>(c))
      sh_.run_err_trap(st);
    if (sh_.opt_errexit) { sh_.exiting = true; sh_.exit_status = st; }
  }
  return st;
}

int Executor::run_connection(const Connection *c) {
  switch (c->conn) {
    case Connector::Pipe:
      return run_pipeline(c);
    case Connector::And: {
      sh_.errexit_suppress++;
      int st = run(c->first.get());
      sh_.errexit_suppress--;
      if (st == 0 && !unwinding()) st = run(c->second.get());
      return st;
    }
    case Connector::Or: {
      sh_.errexit_suppress++;
      int st = run(c->first.get());
      sh_.errexit_suppress--;
      if (st != 0 && !unwinding()) st = run(c->second.get());
      return st;
    }
    case Connector::Amp: {
      // A bare job spec with `&' (`%1 &') is a synonym for `bg %1': resume the
      // job in the background rather than launching a new child.
      if (auto *sc = dynamic_cast<const SimpleCommand *>(c->first.get())) {
        if (sc->redirects.empty() && sc->words.size() == 1 &&
            !sc->words[0].text.empty() && sc->words[0].text[0] == '%') {
          std::vector<std::string> bgargv = {"bg", sc->words[0].text};
          int status = 0;
          run_builtin(sh_, bgargv, &status);
          return (sh_.last_status = status);
        }
      }
      // Background the first command in its own process group.
      std::string cmd = to_string(c->first.get());
      bool jc = sh_.job_control;
      pid_t pid = fork();
      if (pid == 0) {
        if (jc) setpgid(0, 0);
        signal(SIGINT, SIG_DFL);
        signal(SIGQUIT, SIG_DFL);
        signal(SIGTSTP, SIG_DFL);
        sh_.job_control = false;  // background: descendants must not touch the tty
        sh_.subshell_level++;
        sh_.traps.erase("CHLD");  // the parent fires CHLD when it reaps this job
        // Only ERR here, not drop_child_traps(): gnash fires a background job's
        // own DEBUG trap in the CHILD, where bash fires it in the parent before
        // forking.  Dropping DEBUG here would lose that trap altogether rather
        // than merely stop it being inherited.
        if (!sh_.opt_functrace) sh_.traps.erase("ERR");  // not inherited without -E
        sh_.pending_sigchld = 0;
        Executor ex(sh_);
        int s = ex.run(c->first.get());
        std::fflush(nullptr);
        _exit(s & 0xff);
      }
      if (jc) setpgid(pid, pid);
      sh_.last_bg_pid = pid;
      Shell::Job *j = sh_.add_job(pid, {pid}, cmd, true);
      if (sh_.interactive) std::fprintf(stderr, "[%d] %ld\n", j->id, static_cast<long>(pid));
      if (c->second && !unwinding()) return run(c->second.get());
      return 0;
    }
    case Connector::Semi:
    case Connector::Newline: {
      run(c->first.get());
      if (c->second && !unwinding()) return run(c->second.get());
      return sh_.last_status;
    }
  }
  return 0;
}

int Executor::run_pipeline(const Connection *c) {
  std::vector<const Command *> stages;
  gather_pipeline(c, stages);
  size_t n = stages.size();
  int prev_read = -1;
  std::vector<pid_t> pids;
  long pgid = 0;
  // `shopt -s lastpipe' (with job control off) runs the final stage in the
  // current shell, so its assignments and other side effects persist.
  bool do_lastpipe = n > 1 && !sh_.job_control &&
                     sh_.shopt_opts.count("lastpipe") && sh_.shopt_opts.at("lastpipe");
  bool ran_lastpipe = false;
  int lastpipe_status = 0;
  for (size_t i = 0; i < n; i++) {
    int pipefd[2] = {-1, -1};
    if (i + 1 < n) {
      if (pipe(pipefd) != 0) break;
    }
    if (i == n - 1 && do_lastpipe) {
      // Final stage in-process: read from the previous pipe on stdin, run, then
      // restore stdin.  Not added to `pids' -- it is not a child.
      int saved_in = dup(0);
      // When stdin was closed, the pipe's read end may itself be fd 0; in that
      // case it is already in place, so must not be dup'd-then-closed.
      if (prev_read != -1 && prev_read != 0) {
        dup2(prev_read, 0);
        close(prev_read);
      }
      prev_read = -1;
      lastpipe_status = run(stages[i]);
      ran_lastpipe = true;
      if (saved_in != -1) { dup2(saved_in, 0); close(saved_in); }
      else close(0);  // restore the closed-stdin state
      break;
    }
    pid_t pid = fork();
    if (pid == 0) {
      pid_t me = getpid();
      if (sh_.job_control) setpgid(me, pgid == 0 ? me : static_cast<pid_t>(pgid));
      if (sh_.job_control) {
        signal(SIGINT, SIG_DFL);
        signal(SIGQUIT, SIG_DFL);
        signal(SIGTSTP, SIG_DFL);
        signal(SIGTTIN, SIG_DFL);
        signal(SIGTTOU, SIG_DFL);
      }
      // With stdin (or stdout) closed in the shell, pipe() hands out fd 0 (or
      // 1) as a pipe end, so the end may already BE the target of the dup2 --
      // a blind dup2-then-close would close the fd just put in place.  Guard
      // exactly as bash's do_piping does: `if (pipe_in > 0) close', and for
      // the write side `if (pipe_out == 0 || pipe_out > 1) close'.
      if (prev_read != -1) {
        dup2(prev_read, 0);
        if (prev_read > 0) close(prev_read);
      }
      if (i + 1 < n) {
        close(pipefd[0]);
        dup2(pipefd[1], 1);
        if (pipefd[1] == 0 || pipefd[1] > 1) close(pipefd[1]);
      }
      sh_.job_control = false;  // pipeline stage: no nested tty control
      // A pipeline stage is a subshell: without errtrace it does not inherit the
      // ERR trap (the whole pipeline fires it once in the parent instead), and
      // it never inherits the parent's EXIT trap -- only one it sets itself runs.
      // Not drop_child_traps(): as for a background job, gnash fires a stage's
      // own DEBUG trap in the child, so dropping DEBUG here would lose it.
      if (!sh_.opt_functrace) sh_.traps.erase("ERR");
      sh_.traps.erase("EXIT");
      sh_.traps.erase("CHLD");  // the parent fires CHLD for the stage as a whole
      sh_.pending_sigchld = 0;
      // A simple command as a pipeline stage does not raise $BASH_SUBSHELL; a
      // compound one does.  An explicit ( ) subshell counts itself in
      // run_subshell, so don't double-count it here.
      if (!dynamic_cast<const SimpleCommand *>(stages[i]) &&
          !dynamic_cast<const Subshell *>(stages[i]))
        sh_.subshell_level++;
      Executor ex(sh_);
      int s = ex.run(stages[i]);
      // Run the stage's own EXIT trap, if it installed one, before exiting.
      auto eit = sh_.traps.find("EXIT");
      if (eit != sh_.traps.end()) {
        std::string cmd = eit->second;
        sh_.traps.erase(eit);
        sh_.last_status = s;
        sh_.exiting = false;
        sh_.run_string(cmd);
        if (sh_.exiting) s = sh_.exit_status;
      }
      std::fflush(nullptr);
      _exit(s & 0xff);
    }
    if (pgid == 0) pgid = pid;
    if (sh_.job_control) setpgid(pid, static_cast<pid_t>(pgid));
    pids.push_back(pid);
    if (prev_read != -1) close(prev_read);
    if (i + 1 < n) { close(pipefd[1]); prev_read = pipefd[0]; }
  }
  if (prev_read != -1) close(prev_read);

  if (sh_.job_control) tcsetpgrp(sh_.job_terminal, static_cast<pid_t>(pgid));
  int last_st = 0, pipefail_st = 0;
  bool any_stopped = false;
  std::vector<int> pstat;  // per-stage status, in pipeline order, for $PIPESTATUS
  for (size_t i = 0; i < pids.size(); i++) {
    int wst = 0;
    waitpid(pids[i], &wst, WUNTRACED);
    if (WIFSTOPPED(wst)) { any_stopped = true; pstat.push_back(128 + WSTOPSIG(wst)); continue; }
    sh_.note_child_reaped();  // a pipeline stage that terminated
    int s = WIFEXITED(wst) ? WEXITSTATUS(wst) : (128 + WTERMSIG(wst));
    pstat.push_back(s);
    if (i == pids.size() - 1) last_st = s;
    if (s != 0) pipefail_st = s;  // track the last (rightmost) non-zero stage
  }
  // The in-process last stage is the rightmost of all: it sets the pipeline
  // status, and (for pipefail) overrides only when it too failed.
  if (ran_lastpipe) {
    pstat.push_back(lastpipe_status);
    last_st = lastpipe_status;
    if (lastpipe_status != 0) pipefail_st = lastpipe_status;
  }
  // Publish $PIPESTATUS (one element per stage, left to right).
  {
    std::vector<std::pair<std::optional<std::string>, std::string>> elems;
    for (int s : pstat) elems.emplace_back(std::nullopt, std::to_string(s));
    sh_.array_assign("PIPESTATUS", elems, false, false);
  }
  // Normally the pipeline's status is the last stage's; under `set -o pipefail'
  // it is the last stage to exit non-zero (0 if all succeeded), so an upstream
  // failure is not masked by a later success.
  int st = sh_.opt_pipefail ? pipefail_st : last_st;
  if (sh_.job_control) tcsetpgrp(sh_.job_terminal, static_cast<pid_t>(sh_.shell_pgid));

  if (any_stopped) {
    std::vector<long> lp(pids.begin(), pids.end());
    Shell::Job *j = sh_.add_job(pgid, lp, to_string(c), false);
    j->stopped = true;
    j->running = false;
    sh_.set_current_job(j->id);  // bash: a job that stops becomes the current job
    if (sh_.interactive) {
      std::fprintf(stderr, "\n[%d]+  Stopped                 %s\n", j->id, j->command.c_str());
      j->notified = true;  // bash J_NOTIFIED: don't report again at the next prompt
    }
    st = 128 + SIGTSTP;
  }

  if (c->flags & CMD_INVERT_RETURN) st = st ? 0 : 1;
  sh_.last_status = st;
  // A pipeline that returns non-zero triggers errexit (e.g. `set -e; true|false').
  // Suppressed contexts (conditions, `&&'/`||' non-final operands, `!') are
  // handled by errexit_suppress and the CMD_INVERT_RETURN guard in run().
  if (st != 0 && sh_.errexit_suppress == 0 && !unwinding() &&
      !(c->flags & CMD_INVERT_RETURN)) {
    sh_.run_err_trap(st);
    if (sh_.opt_errexit) { sh_.exiting = true; sh_.exit_status = st; }
  }
  return st;
}

// bash's assign_in_env(): a temporary assignment whose name is a nameref binds
// the nameref's TARGET, so `ref=xxx cmd' puts var=xxx in the environment and
// leaves `ref' itself alone.  A nameref with no target, or one aimed at an
// array element, keeps its own name (bash's valid_nameref_value rejects an
// array reference here because the name is used to create a variable directly).
std::string temp_assign_name(Shell &sh, const std::string &name) {
  auto it = sh.vars.find(name);
  if (it == sh.vars.end() || !it->second.nameref) return name;
  // A CIRCULAR chain has no target to resolve to: keep the name as written so
  // the assignment reports the cycle against it (`v->w->x->v; x=4' warns about
  // `x'), rather than against whichever link a bounded walk happened to stop
  // on.
  bool circular = false, too_deep = false;
  std::string t = sh.deref_ex(name, circular, &too_deep);
  // A chain that is circular or too long has no target to bind: keep the name
  // as written so Shell::set reports it against what the user actually wrote.
  if (circular || too_deep || t == name || t.find('[') != std::string::npos) return name;
  return t;
}

int Executor::run_simple(const SimpleCommand *c) {
  if (c->line > 0) sh_.cur_lineno = sh_.lineno_base + c->line;  // $LINENO
  // $BASH_COMMAND tracks the command currently executing (bash sets it before
  // every command, not only inside a DEBUG trap) -- except while a trap body
  // runs, where it stays the command that TRIGGERED the trap for the whole
  // body, so the handler can report what failed.
  if (!sh_.in_err_trap) sh_.bash_command = to_string(c);
  // Consume the exec-in-place permission for *this* command up front, so it
  // applies only to a direct external here -- never to commands that a builtin
  // (eval/source) or function invoked by this command goes on to run.
  bool exec_replace = sh_.can_exec_replace;
  sh_.can_exec_replace = false;
  // DEBUG trap: fires before the command, with $BASH_COMMAND set.  If the trap
  // runs `return'/`exit', skip the command and let the unwind propagate.
  if (sh_.traps.count("DEBUG") && !sh_.in_debug_trap) {
    int tst = sh_.run_debug_trap(to_string(c));
    if (unwinding()) return sh_.last_status;
    // shopt -s extdebug: a non-zero DEBUG trap status skips the command.
    auto ed = sh_.shopt_opts.find("extdebug");
    if (tst != 0 && ed != sh_.shopt_opts.end() && ed->second)
      return sh_.last_status;
  }
  Expander ex(sh_);
  // Reap any <(...) / >(...) set up for this command once it (and any function
  // body it invokes) has finished, on every return path.
  struct ProcsubGuard {
    Shell &s;
    size_t base;
    ~ProcsubGuard() { s.reap_procsubs(base); }
  } psg{sh_, sh_.procsubs.size()};
  std::vector<std::pair<std::string, std::string>> assigns;
  // The first assignment that could not be made (readonly target, invalid
  // nameref value).  Replaying earlier assignments to expand a later RHS
  // reports it, so remember which one it was and do not report it twice.
  size_t first_bad_assign = std::string::npos;
  std::vector<Assign> pending_elem;  // subscripted prefix assigns, decided below
  std::vector<std::string> argv;
  std::vector<Shell::RawArg> raw_prov;
  // Pre-formatted `set -x' trace lines for the assignment words, in source
  // order: a scalar assignment as NAME=<quoted-value> (or NAME+=...), an array
  // compound assignment as its verbatim source word, so xtrace matches bash even
  // for values needing single- or ANSI-C quoting.  Only built when xtrace is on.
  std::vector<std::string> xtrace_lines;
  bool prefix = true;
  // Track command substitutions in the assignment RHS: a pure-assignment
  // command takes the status of the last one (bash), or 0 if there were none.
  sh_.cmdsub_ran = false;
  for (const Word &w : c->words) {
    // Under `set -k' (keyword mode) an assignment-form word ANYWHERE in the
    // command is an assignment, not just those preceding the command name.
    // Checked at run time (the flag can be toggled mid-script) via the word
    // text, since the parser only marks leading words as W_ASSIGNMENT.
    bool assign_here = (prefix && (w.flags & W_ASSIGNMENT)) ||
                       (sh_.opt_keyword && is_assignment_word_text(w.text));
    if (assign_here) {
      Assign a;
      parse_assign(w.text, a);
      if (a.sub) {
        // A SUBSCRIPTED prefix assignment is only valid when no command word
        // follows; decided after the loop (`var[0]=X f' is rejected, bare
        // `var[0]=X' assigns).
        pending_elem.push_back(a);
      } else if (a.is_array) {
        if (sh_.opt_xtrace) {
          // bash traces a compound assignment via its word-list deparser: the
          // PARSED elements re-printed -- `$' '' shows as `' '' (lex-time
          // ANSI expansion), `$@' stays unexpanded -- single-space separated
          // (set-x2.sub).
          std::string line = a.name + (a.append ? "+=(" : "=(");
          bool first = true;
          std::string inner = a.value.size() >= 2 ? a.value.substr(1, a.value.size() - 2)
                                                  : std::string();
          for (const Token &tk : tokenize(inner)) {
            if (tk.type != Tok::Word || tk.text.empty()) continue;
            if (!first) line += ' ';
            line += canonical_word_text(tk.text);
            first = false;
          }
          line += ')';
          xtrace_lines.push_back(line);
        }
        apply_array_assign(sh_, ex, a);  // array literal: applied now
      } else {
        auto vit = sh_.vars.find(a.name);
        // A value assigned to a targetless nameref becomes its referent NAME,
        // not a number, so it must not be arithmetic-evaluated even when the
        // nameref carries -i: bash keeps `7*6' raw so the invalid-identifier
        // diagnostic (Shell::set) quotes it verbatim rather than `42'.
        bool integer = vit != sh_.vars.end() && vit->second.integer &&
                       !(vit->second.nameref && vit->second.value.empty());
        // A nameref inherits its TARGET's integer attribute (for an element
        // target, the base array's): `declare -ai a; declare -n b=a[0]; b+=1'
        // adds arithmetically on a[0] rather than string-appending, as bash does.
        if (vit != sh_.vars.end() && vit->second.nameref && !vit->second.value.empty()) {
          std::string tgt = sh_.deref(a.name);
          size_t lb = tgt.find('[');
          auto tv = sh_.vars.find(lb == std::string::npos ? tgt : tgt.substr(0, lb));
          if (tv != sh_.vars.end() && tv->second.integer) integer = true;
        }
        // A plain `name=value' / `name+=value' where name is already an array
        // targets element 0 (bash), so read/write that element rather than the
        // scalar field.
        bool is_arr = vit != sh_.vars.end() &&
                      (vit->second.kind == VarKind::Indexed || vit->second.kind == VarKind::Assoc);
        // A preceding assignment in the same command is visible to this RHS
        // (`A=1 B=$A'): apply the already-collected assignments, expand, then
        // roll them back so command arguments still see the original values.
        std::vector<std::pair<std::string, std::optional<Variable>>> prior;
        for (size_t pi = 0; pi < assigns.size(); pi++) {
          const auto &pa = assigns[pi];
          auto pv = sh_.vars.find(pa.first);
          prior.push_back({pa.first, pv == sh_.vars.end() ? std::nullopt
                                                          : std::optional<Variable>(pv->second)});
          if (!sh_.set(pa.first, pa.second) && first_bad_assign == std::string::npos)
            first_bad_assign = pi;
        }
        std::string v = ex.expand_assignment(a.value);
        // The traced value is the expanded RHS as written (for `+=' the appended
        // part, not the concatenation); overwritten with the result for integers.
        std::string xtrace_rhs = v;
        // Only the old value is needed for `+=' / integer arithmetic; reading it
        // for a plain assignment would spuriously resolve a circular nameref.
        std::string cur = (integer || a.append)
                              ? (is_arr ? sh_.array_get(a.name, "0") : sh_.get_quiet(a.name))
                              : std::string();
        for (auto it = prior.rbegin(); it != prior.rend(); ++it) {
          if (it->second) sh_.vars[it->first] = *it->second;
          else sh_.vars.erase(it->first);
        }
        if (integer) {
          // An integer-attributed assignment evaluates the RHS as arithmetic;
          // a malformed value (`i=0#4') prints bash's diagnostic and aborts the
          // assignment with status 1 rather than silently storing 0.
          bool ok = true;
          long long rv = eval_arith_msg(sh_, v, "", &ok);
          if (ok && a.append) rv = eval_arith_msg(sh_, cur, "", &ok) + rv;
          if (!ok) { sh_.arith_error = true; break; }
          v = std::to_string(rv);
          xtrace_rhs = v;
        } else if (a.append) {
          v = cur + v;
        }
        if (sh_.opt_xtrace)
          xtrace_lines.push_back(a.name + (a.append ? "+=" : "=") +
                                 (xtrace_rhs.empty() ? std::string()
                                                     : xtrace_quote_word(xtrace_rhs)));
        if (is_arr) sh_.array_set(a.name, "0", v);
        else assigns.emplace_back(temp_assign_name(sh_, a.name), v);
      }
    } else {
      prefix = false;
      // For an assignment builtin (declare/local/readonly/typeset), a name=value
      // argument is an assignment word: pass it through raw so it is neither
      // word-split nor globbed and array literals survive; the builtin expands
      // and parses it itself.
      if (!argv.empty() && is_assignment_builtin(argv[0]) && is_assignment_word_text(w.text)) {
        argv.push_back(w.text);
        raw_prov.push_back({w.text, (w.flags & W_QUOTED) != 0,
                            is_compound_assignment_source(w.text)});
      } else {
        size_t before = argv.size();
        for (const std::string &f : ex.expand_args({w})) argv.push_back(f);
        // Provenance only survives a 1:1 word->field expansion.
        if (argv.size() == before + 1)
          raw_prov.push_back({w.text, (w.flags & W_QUOTED) != 0});
        raw_prov.resize(argv.size());
      }
    }
  }

  // Subscripted assignments in a temporary environment are invalid: with a
  // command word present bash rejects each (`var[0]=X f' prints `var[0]':
  // not a valid identifier) and runs the command without them; with no
  // command word they are ordinary element assignments.
  for (const Assign &a : pending_elem) {
    if (!argv.empty()) {
      std::string disp = a.name + "[" + (a.sub ? *a.sub : std::string()) + "]";
      std::fprintf(stderr, "%s`%s': not a valid identifier\n", sh_.err_prefix().c_str(),
                   disp.c_str());
    } else {
      apply_array_assign(sh_, ex, a);
    }
  }

  // A failed arithmetic expansion (bad expression, or assignment to a readonly
  // variable) during word expansion aborts the command with status 1; the
  // shell continues.
  if (sh_.arith_error) {
    sh_.arith_error = false;
    return (sh_.last_status = 1);
  }
  // An expansion that began an unwind (`set -u' unbound variable, an
  // aborting arithmetic error) suppresses the command itself: bash never
  // runs the echo in `( echo ${#narray[4]} )'.
  if (sh_.exiting) return sh_.last_status = sh_.exit_status;
  if (unwinding()) return sh_.last_status;

  if (sh_.opt_xtrace) {
    // bash's xtrace prefix is $PS4 (default `+ '), decoded for prompt escapes
    // then word-expanded, so `$LINENO' and friends resolve for the traced line;
    // its first character repeats once per nesting level.
    std::string xt_prefix = sh_.xtrace_prefix();
    // bash traces each temporary/standalone assignment on its own line, then the
    // command word list (if any) on a separate line.  Assignment lines were
    // pre-formatted in source order (scalar values and command words are quoted
    // as bash's sh_single_quote/ANSI-C xtrace conventions require).
    for (const auto &a : xtrace_lines)
      std::fprintf(sh_.xtrace_out(), "%s%s\n", xt_prefix.c_str(), a.c_str());
    if (!argv.empty()) {
      std::string line = xt_prefix;
      bool first = true;
      for (const auto &a : argv) {
        if (!first) line += ' ';
        line += xtrace_quote_word(a);
        first = false;
      }
      std::fprintf(sh_.xtrace_out(), "%s\n", line.c_str());
    }
  }

  // `command [-pvV] NAME [args...]': run NAME as a builtin or external, bypassing
  // any shell function of the same name.  The execution case is handled here --
  // not in the `command' builtin -- so NAME reuses the normal redirect / temporary-
  // assignment / job-control path with the already-expanded, correctly-quoted argv.
  // (The builtin re-joined the words into a string and re-parsed them, which
  // corrupted quoting and any embedded shell metacharacters.)  The describe forms
  // -v/-V, and invalid options, are left for the builtin's `command' case, where
  // the name-lookup helpers live: we detect them and simply don't strip.
  bool skip_functions = false;
  while (!argv.empty() && argv[0] == "command") {
    size_t k = 1;
    bool describe = false, bad = false;
    for (; k < argv.size(); k++) {
      const std::string &o = argv[k];
      if (o == "--") { k++; break; }
      if (o.size() < 2 || o[0] != '-') break;
      for (size_t j = 1; j < o.size(); j++) {
        if (o[j] == 'v' || o[j] == 'V') describe = true;
        else if (o[j] == 'p') {  // default PATH; forbidden in a restricted shell
          if (sh_.opt_restricted) {
            std::fprintf(stderr, "%scommand: -p: restricted\n", sh_.err_prefix().c_str());
            return (sh_.last_status = 2);
          }
        }
        else { bad = true; break; }
      }
      if (bad) break;
    }
    if (describe || bad) break;  // -v/-V/invalid: dispatch to the `command' builtin
    argv.erase(argv.begin(), argv.begin() + k);  // strip `command' and its options
    raw_prov.erase(raw_prov.begin(),
                   raw_prov.begin() + std::min(k, raw_prov.size()));
    skip_functions = true;
    if (argv.empty()) return (sh_.last_status = 0);  // bare `command'
  }

  // No command word: assignments take effect in the current shell.  The status
  // is that of the last command substitution in the RHS, or 0 if there was none.
  if (argv.empty()) {
    std::vector<SavedFd> saved;
    apply_redirects(sh_, c->redirects, saved);
    // A failed assignment in a command with NO command word is a fatal
    // assignment error: bash longjmps out of the current top-level command
    // list (`RO=z; echo hi' prints nothing, and the abort escapes functions and
    // loops), while the reader carries on with the next line.  With a command
    // word (`RO=z echo hi') the command still runs and nothing is abandoned.
    // Assignments before the failing one still take effect (`a=1 RO=z b=2'
    // leaves a set and b unset); the rest are abandoned.
    bool assign_failed = false;
    for (size_t ai = 0; ai < assigns.size() && !assign_failed; ai++) {
      if (ai == first_bad_assign) { assign_failed = true; break; }  // already reported
      if (!sh_.set(assigns[ai].first, assigns[ai].second)) assign_failed = true;
    }
    restore_fds(saved);
    if (assign_failed) {
      sh_.arith_abort = true;
      return (sh_.last_status = 1);
    }
    int st = sh_.cmdsub_ran ? sh_.last_cmdsub_status : 0;
    if (c->flags & CMD_INVERT_RETURN) st = st ? 0 : 1;  // a bare `!'
    sh_.last_status = st;
    // A failing command substitution in the RHS (`x=$(false)') triggers errexit
    // just like any other command's non-zero status.
    if (st != 0 && sh_.errexit_suppress == 0 && !unwinding() &&
        !(c->flags & CMD_INVERT_RETURN)) {
      sh_.run_err_trap(st);
      if (sh_.opt_errexit) { sh_.exiting = true; sh_.exit_status = st; }
    }
    return st;
  }

  // A bare job spec in command position (`%1', `%', `%%', `%-', `%name') is a
  // synonym for `fg %spec'.  (The backgrounding form `%spec &' is turned into
  // `bg %spec' in run_connection before we ever get here.)
  if (argv.size() == 1 && argv[0][0] == '%') {
    std::vector<std::string> fgargv = {"fg", argv[0]};
    int status = 0;
    run_builtin(sh_, fgargv, &status);
    return (sh_.last_status = status);
  }

  // A restricted shell forbids a command name containing `/' (a builtin or
  // function of that literal name would already have matched by exact name).
  if (sh_.opt_restricted && argv[0].find('/') != std::string::npos &&
      sh_.functions.find(argv[0]) == sh_.functions.end()) {
    std::fprintf(stderr, "%s%s: restricted: cannot specify `/' in command names\n",
                 sh_.err_prefix().c_str(), argv[0].c_str());
    return (sh_.last_status = 126);
  }

  // Builtins and functions run in-process (with redirects applied/restored).
  auto fit = sh_.functions.find(argv[0]);
  bool is_func = !skip_functions && fit != sh_.functions.end();
  // Posix command search order finds special builtins BEFORE functions: with
  // a `break' function defined, posix-mode `break' runs the builtin.
  if (is_func && sh_.opt_posix && is_special_builtin_name(argv[0]) &&
      !sh_.disabled_builtins.count(argv[0]))
    is_func = false;
  int dummy = 0;
  bool builtin = false;
  {
    // peek: is it a builtin name? run_builtin decides.
    // We apply redirects first, then dispatch.
  }
  (void)dummy;

  std::vector<SavedFd> saved;
  // A `{var}' target that cannot be assigned is blamed on `exec' when the
  // redirection is exec's own; the same redirection on a compound command is
  // reported bare (bash).
  const char *fdvar_ctx =
      (!argv.empty() && argv[0] == "exec") ? "exec" : nullptr;
  if (!apply_redirects(sh_, c->redirects, saved, fdvar_ctx)) {
    restore_fds(saved);
    return (sh_.last_status = 1);
  }

  // Temporary assignments: set as shell vars for the command and *exported* so
  // child processes spawned by a function see them, then fully restored after
  // (for builtins/functions).  For external commands they go into the env.
  std::vector<std::pair<std::string, std::optional<Variable>>> restore;
  auto apply_temp = [&]() {
    for (const auto &a : assigns) {
      sh_.temp_env_active.insert(a.first);  // let a called function's `local' inherit it
      sh_.temp_env_depth[a.first]++;
      auto it = sh_.vars.find(a.first);
      restore.push_back({a.first,
                         it == sh_.vars.end() ? std::nullopt : std::optional<Variable>(it->second)});
      // Record the pre-temp binding: a `local'/`declare' created during the
      // command CONSUMES the temp layer (make_local), taking this as its
      // frame-restore point so `v=t declare -x v' in a function leaves the
      // caller's v untouched while the local keeps t until return.
      sh_.temp_prior[a.first] = restore.back().second;
      if (it != sh_.vars.end() && it->second.nameref) {
        // A temporary assignment to a nameref shadows it with a plain binding
        // for the duration of the command; it does not write through to the
        // target variable (bash discards the temp binding afterward, leaving
        // the nameref and its target unchanged).
        Variable v;
        v.value = a.second;
        v.exported = true;
        sh_.vars[a.first] = v;
      } else {
        sh_.set(a.first, a.second);
        sh_.vars[a.first].exported = true;  // visible to the command's children
      }
    }
  };
  auto undo_temp = [&]() {
    for (auto it = restore.rbegin(); it != restore.rend(); ++it) {
      if (sh_.temp_consumed.count(it->first)) continue;  // localized: frame owns it
      // A posix special builtin persisted this binding through us: keep the
      // value it set and consume the mark (`var=30 func' + `var=20 return').
      if (sh_.temp_persist.count(it->first)) {
        sh_.temp_persist.erase(it->first);
        continue;
      }
      if (it->second) sh_.vars[it->first] = *it->second;
      else sh_.vars.erase(it->first);
    }
    restore.clear();
    bool relocale = false;
    for (const auto &a : assigns) {
      sh_.temp_env_active.erase(a.first);
      sh_.temp_prior.erase(a.first);
      sh_.temp_consumed.erase(a.first);
      auto d = sh_.temp_env_depth.find(a.first);
      if (d != sh_.temp_env_depth.end() && --d->second <= 0) sh_.temp_env_depth.erase(d);
      // Applying `LC_ALL=C printf ...' went through Shell::set (which resets
      // the locale); the teardown above wrote vars directly, so re-derive.
      if (a.first == "LC_ALL" || a.first == "LC_CTYPE" || a.first == "LC_NUMERIC" ||
          a.first == "LANG")
        relocale = true;
    }
    if (relocale) sh_.reapply_locale();
  };

  int status = 0;
  if (is_func) {
    // FUNCNEST caps function-call nesting: bash aborts with an error once the
    // depth would reach it (a value <= 0 or an unset/invalid FUNCNEST = no cap).
    if (sh_.is_set("FUNCNEST")) {
      long fn_max = std::strtol(sh_.get("FUNCNEST").c_str(), nullptr, 10);
      if (fn_max > 0 && static_cast<long>(sh_.local_stack.size()) >= fn_max) {
        std::fprintf(stderr, "%s%s: maximum function nesting level exceeded (%ld)\n",
                     sh_.err_prefix().c_str(), argv[0].c_str(), fn_max);
        sh_.last_status = 1;
        return 1;
      }
    }
    apply_temp();
    std::vector<std::string> saved_pos = sh_.positional;
    sh_.positional.assign(argv.begin() + 1, argv.end());
    // Record the call for `caller': line of the call site, the function name,
    // and the source.
    sh_.call_stack.push_back({sh_.cur_lineno, argv[0],
                              sh_.shell_name.empty() ? "main" : sh_.shell_name});
    // BASH_SOURCE/FUNCNAME frame: the function runs in the file it was defined
    // in; the call line is where it was invoked in the current file.
    auto fsit = sh_.func_src.find(argv[0]);
    std::string def_src = (fsit != sh_.func_src.end()) ? fsit->second : sh_.current_source();
    sh_.push_src_frame(argv[0], def_src, sh_.cur_lineno, true);
    // BASH_ARGC/BASH_ARGV call-argument stack (extdebug only).  At the outermost
    // call, snapshot the script's positionals for the trailing "main" frame.
    bool pushed_argframe = sh_.opt_extdebug;
    if (pushed_argframe) {
      if (sh_.argframes.empty()) sh_.top_positionals = saved_pos;
      sh_.argframes.emplace_back(argv.begin() + 1, argv.end());
    }
    sh_.push_scope();
    sh_.debug_frame.push_back(sh_.traps.count("DEBUG") ? sh_.traps["DEBUG"] : std::string());
    sh_.return_frame.push_back(sh_.traps.count("RETURN") ? sh_.traps["RETURN"] : std::string());
    // `declare -ft NAME' traces this function specifically: it inherits the
    // DEBUG and RETURN traps as though functrace were on.
    sh_.traced_frame.push_back(sh_.traced_functions.count(argv[0]) > 0);
    sh_.persona_restore.push_back(std::nullopt);  // for `personality -L' / `emulate -L'
    // Run the body under the lineno_base captured at definition time so $LINENO
    // reports absolute source lines regardless of the caller's input block.
    int saved_lineno_base = sh_.lineno_base;
    auto lbit = sh_.func_lineno_base.find(argv[0]);
    if (lbit != sh_.func_lineno_base.end()) sh_.lineno_base = lbit->second;
    // A `break'/`continue' in the function body refers to a loop in that body,
    // not the caller's -- bash saves and zeroes loop_level across the call.
    int saved_loop_depth = sh_.loop_depth;
    sh_.loop_depth = 0;
    // Under functrace, bash fires the DEBUG trap once for the function body as a
    // whole on entry, before the per-command traps inside it.
    if ((sh_.opt_functrace || sh_.in_traced_function()) && sh_.traps.count("DEBUG") &&
        !sh_.in_debug_trap) {
      // The whole-body DEBUG trap reports the function's definition line.
      auto dlit = sh_.func_def_line.find(argv[0]);
      sh_.cur_lineno = dlit != sh_.func_def_line.end() ? dlit->second
                                                       : sh_.lineno_base + 1;
      sh_.run_debug_trap(to_string(fit->second));
    }
    // Entering the body, bash sets `line_number = function_line_number =
    // tc->line' -- the line the body STARTS on (execute_function).  That is
    // the ambient line inside the function for any construct that does not
    // install its own (a select's identifier check, redirection errors on
    // if/while/case, ...).
    if (fit->second && fit->second->line > 0) {
      auto blit = sh_.func_lineno_base.find(argv[0]);
      sh_.cur_lineno = (blit != sh_.func_lineno_base.end() ? blit->second
                                                           : sh_.lineno_base) +
                       fit->second->line;
    }
    status = unwinding() ? sh_.last_status : run(fit->second);
    // Deliver a signal caught during the body while the function scope is still
    // active, so its trap runs in-context (bash): a `return' in the trap then
    // returns from THIS function rather than erroring at the outer level.
    if (!unwinding()) sh_.run_pending_traps();
    // The RETURN trap fires when the function returns, in its own scope, with $?
    // set to the return status; return_trap_fires() holds bash's rule for when.
    {
      // Leaving the body, bash reports the function's DEFINITION line again --
      // the same line its entry DEBUG trap named -- rather than whatever the
      // last command inside happened to set.
      auto rlit = sh_.func_def_line.find(argv[0]);
      if (rlit != sh_.func_def_line.end()) sh_.cur_lineno = rlit->second;
      if (sh_.return_trap_fires()) {
        int ret_status = sh_.returning ? sh_.exit_status : status;
        bool save_ret = sh_.returning;
        int save_es = sh_.exit_status;
        sh_.returning = false;  // let the trap body run rather than unwind
        sh_.run_return_trap(ret_status);
        sh_.returning = save_ret;
        sh_.exit_status = save_es;
      }
    }
    sh_.lineno_base = saved_lineno_base;
    sh_.loop_depth = saved_loop_depth;
    if (!sh_.persona_restore.empty()) {
      if (sh_.persona_restore.back()) sh_.set_personality(*sh_.persona_restore.back());
      sh_.persona_restore.pop_back();
    }
    sh_.pop_scope();
    sh_.debug_frame.pop_back();
    sh_.return_frame.pop_back();
    sh_.traced_frame.pop_back();
    if (pushed_argframe && !sh_.argframes.empty()) sh_.argframes.pop_back();
    sh_.pop_src_frame();
    sh_.call_stack.pop_back();
    sh_.positional = saved_pos;
    if (sh_.returning) { sh_.returning = false; status = sh_.exit_status; }
    // Assignments preceding a function call are UNDONE at return, in posix
    // mode too: bash no longer propagates `var=two func' to the caller, even
    // when the function export/readonly-marks the temporary (varenv12.sub).
    undo_temp();
  } else if ((apply_temp(), [&] {
               // `command' strips the POSIX special-builtin exit property: a
               // failing `command . nofile' does not end a posix-mode shell.
               sh_.posix_builtin_shield = skip_functions;
               sh_.raw_args = std::move(raw_prov);
               bool b = run_builtin(sh_, argv, &status);
               sh_.raw_args.clear();
               sh_.posix_builtin_shield = false;
               return b;
             }())) {
    // A preceding `VAR=val builtin' applies to the builtin (e.g. IFS=, read),
    // then is restored.  It keeps effect permanently when the builtin makes the
    // variable readonly or exported: `export'/`readonly' always do, `declare'/
    // `typeset' when given `-r' (readonly) or `-x' (export) -- a temp-env var so
    // marked is promoted to a real, still-exported variable (`x=4 declare -r x'
    // -> `declare -rx x="4"'), where a plain `-i' does not persist.  In posix
    // mode all the POSIX special builtins persist too.
    builtin = true;
    bool persist = argv[0] == "export" || argv[0] == "readonly";
    if (!persist && !sh_.in_function() &&
        (argv[0] == "declare" || argv[0] == "typeset")) {
      // Inside a function, `v=t declare -x v' needs no promotion: declare
      // creates a LOCAL that inherits the temp-env value, and it unwinds at
      // return leaving the caller's binding untouched (varenv12.sub).
      for (size_t k = 1; k < argv.size() && argv[k].size() > 1 && argv[k][0] == '-'; k++)
        if (argv[k].find('x') != std::string::npos ||
            argv[k].find('r') != std::string::npos) { persist = true; break; }
    }
    bool special_persist = false;
    if (sh_.opt_posix) {
      static const std::set<std::string> kSpecial = {
          ":",      ".",     "break", "continue", "eval",  "exec",
          "exit",   "export", "readonly", "return", "set", "shift",
          "source", "times", "trap",  "unset"};
      // In posix mode export/readonly write through an enclosing tempenv TOO
      // (they are special builtins); in default mode their promotion stays
      // within the current command's scope.
      if (kSpecial.count(argv[0])) persist = special_persist = true;
    }
    if (persist) {
      // Under the POSIX-special rule only: a name also bound by an ENCLOSING
      // temporary environment (depth >= 2: ours plus at least one outer)
      // persists through it -- mark it so the outer undo keeps the value this
      // builtin set (`var=30 func' + `var=20 return').  The export/readonly
      // promotion path must NOT punch through: default-mode `a=7 f1' where f1
      // runs `a=3 readonly a' restores the caller's a (and its attributes)
      // at return (varenv23.sub).
      if (special_persist)
        for (const auto &a : assigns) {
          auto d = sh_.temp_env_depth.find(a.first);
          if (d != sh_.temp_env_depth.end() && d->second >= 2) sh_.temp_persist.insert(a.first);
        }
      restore.clear();
    }
    undo_temp();
    // A builtin that performs an internal assignment (e.g. `declare -i a[$bad]=x'
    // under array_expand_once) can raise the arithmetic-error flag from deep in
    // the assignment path -- past the pre-dispatch check above.  The builtin has
    // already printed the diagnostic and set its own status, so clear the flag
    // here; otherwise it would spuriously abort the *next* command.
    sh_.arith_error = false;
  } else {
    undo_temp();  // not a builtin after all: the external path sets its own env
    // On an exec failure for a file that EXISTS and starts `#!', bash blames
    // the interpreter: `./x23: nosuchfile: bad interpreter: No such file or
    // directory' -- prefixed by the script name alone, with NO line number
    // (shell_execve).  Returns false when this isn't that case, so the
    // caller falls back to the plain strerror report.
    auto report_bad_interpreter = [&](const std::string &file) -> bool {
      int saved_errno = errno;
      if (saved_errno != ENOENT && saved_errno != EACCES) return false;
      std::FILE *fp = std::fopen(file.c_str(), "r");
      if (fp == nullptr) { errno = saved_errno; return false; }
      char line[256];
      bool ok = std::fgets(line, sizeof line, fp) != nullptr;
      std::fclose(fp);
      errno = saved_errno;
      if (!ok || line[0] != '#' || line[1] != '!') return false;
      std::string interp;
      size_t p = 2;
      while (line[p] == ' ' || line[p] == '\t') p++;
      while (line[p] != '\0' && line[p] != ' ' && line[p] != '\t' &&
             line[p] != '\n')
        interp += line[p++];
      if (interp.empty()) return false;
      // The interpreter itself must be the missing piece; an executable
      // interpreter means the failure was something else.
      if (access(interp.c_str(), X_OK) == 0) return false;
      std::fprintf(stderr, "%s: %s: %s: bad interpreter: %s\n",
                   sh_.shell_name.c_str(), file.c_str(), interp.c_str(),
                   std::strerror(saved_errno));
      return true;
    };
    // A command name without `/' that has been hashed (`hash -p', BASH_CMDS)
    // execs the remembered path, as bash does; execvp() still falls back to a
    // $PATH search for the (unhashed) common case.
    std::string exec_file = argv[0];
    if (argv[0].find('/') == std::string::npos) {
      const std::string *h = sh_.hash_lookup(argv[0]);  // a hit bumps `hash' hits
      if (h != nullptr) {
        exec_file = *h;
        // `shopt -s checkhash': verify the remembered path still names an
        // executable before using it; a stale entry falls back to a fresh
        // $PATH search and is re-remembered (bash).  Without checkhash the
        // stale path is used -- and fails -- exactly as bash does.
        auto ch = sh_.shopt_opts.find("checkhash");
        if (ch != sh_.shopt_opts.end() && ch->second &&
            access(exec_file.c_str(), X_OK) != 0) {
          std::string fresh = find_in_path(sh_, argv[0]);
          if (!fresh.empty()) {
            exec_file = fresh;
            sh_.hash_remember(argv[0], fresh);
          }
        }
      }
    }
    // execvp searches $PATH for a name without `/', or uses a `/' path
    // directly; a hashed name execs its remembered value, and reports that
    // value's name on failure (bash's behavior for `hash'/BASH_CMDS entries).
    auto do_exec = [&](std::vector<char *> &cargv) {
      // Resolve a bare name on $PATH ourselves and execve the full path, rather
      // than execvp -- execvp silently re-runs a #!-less script through /bin/sh,
      // whereas bash (and we) re-exec it with the current shell.
      std::string full = exec_file;
      if (!exec_file.empty() && exec_file.find('/') == std::string::npos) {
        std::string p = sh_.get("PATH");
        bool found = false;
        std::string noexec_path;  // a regular-file match that is not executable
        for (size_t i = 0; i <= p.size();) {
          size_t j = p.find(':', i);
          std::string dir = p.substr(i, j == std::string::npos ? std::string::npos : j - i);
          if (dir.empty()) dir = ".";
          std::string cand = dir + "/" + exec_file;
          struct stat stbuf;
          if (stat(cand.c_str(), &stbuf) == 0 && S_ISREG(stbuf.st_mode)) {
            if (access(cand.c_str(), X_OK) == 0) { full = cand; found = true; break; }
            if (noexec_path.empty()) noexec_path = cand;
          }
          if (j == std::string::npos) break;
          i = j + 1;
        }
        if (!found) {
          // A file that exists on $PATH but is not executable is reported by
          // its full path as "Permission denied" (126); otherwise the bare
          // name is "command not found" (127), as bash does.
          if (!noexec_path.empty()) { exec_file = noexec_path; errno = EACCES; }
          else errno = ENOENT;
          return;
        }
      }
      execve(full.c_str(), cargv.data(), environ);
      // A file that is neither a binary nor has a #! line: run it as a shell
      // script with this shell, as bash does (not the libc /bin/sh fallback).
      if (errno == ENOEXEC && !sh_.self_exe.empty()) {
        std::vector<char *> nargv;
        nargv.push_back(const_cast<char *>(sh_.self_exe.c_str()));
        nargv.push_back(const_cast<char *>(full.c_str()));
        for (size_t k = 1; cargv[k] != nullptr; k++) nargv.push_back(cargv[k]);
        nargv.push_back(nullptr);
        execve(sh_.self_exe.c_str(), nargv.data(), environ);
      }
    };
    // external command.  If we are a disposable subshell child whose sole
    // command this is, exec it in place -- become the command with no second
    // fork/wait (exec_replace was consumed at the top of run_simple).
    if (exec_replace) {
      std::vector<std::string> envs = sh_.environ_block();
      for (const auto &a : assigns) {
        // A temporary assignment overrides any exported value of the same name.
        std::string pre = a.first + "=";
        envs.erase(std::remove_if(envs.begin(), envs.end(),
                                  [&](const std::string &e) { return e.compare(0, pre.size(), pre) == 0; }),
                   envs.end());
        envs.push_back(a.first + "=" + a.second);
      }
      std::vector<char *> envp;
      for (auto &e : envs) envp.push_back(const_cast<char *>(e.c_str()));
      envp.push_back(nullptr);
      environ = envp.data();
      std::vector<char *> cargv;
      for (auto &a : argv) cargv.push_back(const_cast<char *>(a.c_str()));
      cargv.push_back(nullptr);
      std::fflush(nullptr);
      do_exec(cargv);
      if (errno == ENOENT && exec_file.find('/') == std::string::npos)
        std::fprintf(stderr, "%s%s: %s\n", sh_.err_prefix().c_str(), printable_name(exec_file).c_str(),
                     exec_file == argv[0] ? "command not found" : "not found");
      else if (!report_bad_interpreter(exec_file))
        std::fprintf(stderr, "%s%s: %s\n", sh_.err_prefix().c_str(),
                     printable_name(exec_file).c_str(), std::strerror(errno));
      _exit(errno == EACCES ? 126 : 127);
    }
    // external command, in its own process group
    pid_t pid = fork();
    if (pid == 0) {
      if (sh_.job_control) setpgid(0, 0);
      if (sh_.job_control) {
        signal(SIGINT, SIG_DFL);
        signal(SIGQUIT, SIG_DFL);
        signal(SIGTSTP, SIG_DFL);
        signal(SIGTTIN, SIG_DFL);
        signal(SIGTTOU, SIG_DFL);
      }
      std::vector<std::string> envs = sh_.environ_block();
      for (const auto &a : assigns) {
        // A temporary assignment overrides any exported value of the same name.
        std::string pre = a.first + "=";
        envs.erase(std::remove_if(envs.begin(), envs.end(),
                                  [&](const std::string &e) { return e.compare(0, pre.size(), pre) == 0; }),
                   envs.end());
        envs.push_back(a.first + "=" + a.second);
      }
      std::vector<char *> envp;
      for (auto &e : envs) envp.push_back(const_cast<char *>(e.c_str()));
      envp.push_back(nullptr);
      environ = envp.data();
      std::vector<char *> cargv;
      for (auto &a : argv) cargv.push_back(const_cast<char *>(a.c_str()));
      cargv.push_back(nullptr);
      do_exec(cargv);
      if (errno == ENOENT && exec_file.find('/') == std::string::npos)
        std::fprintf(stderr, "%s%s: %s\n", sh_.err_prefix().c_str(), printable_name(exec_file).c_str(),
                     exec_file == argv[0] ? "command not found" : "not found");
      else if (!report_bad_interpreter(exec_file))
        std::fprintf(stderr, "%s%s: %s\n", sh_.err_prefix().c_str(),
                     printable_name(exec_file).c_str(), std::strerror(errno));
      _exit(errno == EACCES ? 126 : 127);
    }
    if (sh_.job_control) setpgid(pid, pid);
    if (sh_.job_control) tcsetpgrp(sh_.job_terminal, pid);
    int wst = 0;
    waitpid(pid, &wst, WUNTRACED);
    if (sh_.job_control) tcsetpgrp(sh_.job_terminal, static_cast<pid_t>(sh_.shell_pgid));
    if (WIFSTOPPED(wst)) {
      std::string cmd;
      for (size_t k = 0; k < argv.size(); k++) { if (k) cmd += ' '; cmd += argv[k]; }
      Shell::Job *j = sh_.add_job(pid, {pid}, cmd, false);
      j->stopped = true;
      j->running = false;
      sh_.set_current_job(j->id);  // bash: a job that stops becomes the current job
      status = 128 + SIGTSTP;
      if (sh_.interactive) {
        std::fprintf(stderr, "\n[%d]+  Stopped                 %s\n", j->id, cmd.c_str());
        j->notified = true;  // bash J_NOTIFIED: don't report again at the next prompt
      }
    } else {
      sh_.note_child_reaped();  // a foreground subshell that terminated
      status = WIFEXITED(wst) ? WEXITSTATUS(wst) : (128 + WTERMSIG(wst));
    }
  }
  (void)builtin;

  // Flush buffered builtin/function output while our redirections are still in
  // effect, so it lands on the right fd and in program order; drop it if the
  // target fd was closed (`>&-') rather than letting it leak out on restore.
  flush_builtin_stdout();
  // `exec' makes its redirections permanent in the current shell (this path is
  // only reached when exec had no command word, or its exec failed).
  if (builtin && !argv.empty() && argv[0] == "exec" && status == 0) {
    // `exec 0< file' while reading commands from stdin swaps the command
    // source: tell the stdin driver to re-read from the new fd 0.
    if (sh_.invocation_char == 's')
      for (const SavedFd &sf : saved)
        if (sf.fd == 0) sh_.stdin_source_changed = true;
    discard_saved_fds(saved);
  }
  else
    restore_fds(saved);
  if (c->flags & CMD_INVERT_RETURN) status = status ? 0 : 1;
  sh_.last_status = status;
  if (status != 0 && sh_.errexit_suppress == 0 && !unwinding() &&
      !(c->flags & CMD_INVERT_RETURN)) {
    sh_.run_err_trap(status);
    if (sh_.opt_errexit) { sh_.exiting = true; sh_.exit_status = status; }
  }
  return status;
}

int Executor::run_coproc(const CoprocCommand *c) {
  // A coprocess runs asynchronously with its stdin and stdout wired to pipes.
  // The shell keeps the other ends open on high, close-on-exec descriptors and
  // publishes them as NAME[0] (read from the coproc) / NAME[1] (write to it),
  // with NAME_PID holding the child's pid.  NAME defaults to COPROC.
  // The parser accepts any word as the name; validate it here as bash's
  // execute_coproc does (`coproc @ { :; }' -> `@': not a valid identifier).
  if (!c->name.empty()) {
    bool ok = std::isalpha(static_cast<unsigned char>(c->name[0])) || c->name[0] == '_';
    for (char ch : c->name)
      if (!(std::isalnum(static_cast<unsigned char>(ch)) || ch == '_')) ok = false;
    if (!ok) {
      std::fprintf(stderr, "%s`%s': not a valid identifier\n", sh_.err_prefix().c_str(),
                   c->name.c_str());
      return (sh_.last_status = 1);
    }
  }
  const std::string name = c->name.empty() ? std::string("COPROC") : c->name;
  int out_pipe[2], in_pipe[2];  // out: coproc->shell;  in: shell->coproc
  if (pipe(out_pipe) < 0) {
    std::fprintf(stderr, "%scoproc: pipe: %s\n", sh_.err_prefix().c_str(), std::strerror(errno));
    return (sh_.last_status = 1);
  }
  // bash's sh_openpipe moves each pipe end to the HIGHEST free descriptor
  // below 64 (move_to_high_fd with maxfd 64) as soon as the pipe is created:
  // the first coproc allocates 63/62 + 61/60 and publishes 63/60; a second
  // one, started with those still open, gets 62/58 (coproc.tests).
  auto move_high64 = [](int fd) {
    int nfds = 64;
    for (nfds--; nfds > 3; nfds--)
      if (fcntl(nfds, F_GETFD) == -1) break;
    if (nfds > 3 && fd != nfds && dup2(fd, nfds) != -1) {
      close(fd);
      return nfds;
    }
    return fd;
  };
  out_pipe[0] = move_high64(out_pipe[0]);
  out_pipe[1] = move_high64(out_pipe[1]);
  if (pipe(in_pipe) < 0) {
    std::fprintf(stderr, "%scoproc: pipe: %s\n", sh_.err_prefix().c_str(), std::strerror(errno));
    close(out_pipe[0]); close(out_pipe[1]);
    return (sh_.last_status = 1);
  }
  in_pipe[0] = move_high64(in_pipe[0]);
  in_pipe[1] = move_high64(in_pipe[1]);
  bool jc = sh_.job_control;
  std::string cmd = to_string(c->body.get());
  pid_t pid = fork();
  if (pid == 0) {
    if (jc) setpgid(0, 0);
    signal(SIGINT, SIG_DFL);
    signal(SIGQUIT, SIG_DFL);
    signal(SIGTSTP, SIG_DFL);
    dup2(in_pipe[0], 0);   // coproc reads its stdin from the shell
    dup2(out_pipe[1], 1);  // coproc writes its stdout to the shell
    close(in_pipe[0]); close(in_pipe[1]);
    close(out_pipe[0]); close(out_pipe[1]);
    sh_.job_control = false;
    sh_.subshell_level++;
    sh_.traps.erase("CHLD");  // the parent fires CHLD when it reaps this job
    if (!sh_.opt_functrace) sh_.traps.erase("ERR");  // not inherited without -E
    sh_.pending_sigchld = 0;
    Executor ex(sh_);
    int s = ex.run(c->body.get());
    std::fflush(nullptr);
    _exit(s & 0xff);
  }
  // Parent: keep the shell's read/write ends (already on their high
  // descriptors) and mark them close-on-exec, as bash does.
  close(in_pipe[0]);
  close(out_pipe[1]);
  int read_fd = out_pipe[0];
  int write_fd = in_pipe[1];
  fcntl(read_fd, F_SETFD, FD_CLOEXEC);
  fcntl(write_fd, F_SETFD, FD_CLOEXEC);
  sh_.coproc_rfd = read_fd;
  sh_.coproc_wfd = write_fd;
  if (jc) setpgid(pid, pid);
  sh_.last_bg_pid = pid;
  Shell::Job *j = sh_.add_job(pid, {pid}, cmd, true);
  if (sh_.interactive) std::fprintf(stderr, "[%d] %ld\n", j->id, static_cast<long>(pid));

  std::vector<std::pair<std::optional<std::string>, std::string>> elems;
  elems.emplace_back(std::string("0"), std::to_string(read_fd));
  elems.emplace_back(std::string("1"), std::to_string(write_fd));
  // A readonly NAME stops the whole thing: bash reports the array assignment
  // and never goes on to NAME_PID.
  auto nit = sh_.vars.find(sh_.deref(name));
  bool name_ro = nit != sh_.vars.end() && nit->second.readonly;
  sh_.array_assign(name, elems, false, false);
  if (!name_ro) sh_.set(name + "_PID", std::to_string(pid));
  // Remember it so the variables go away once the coprocess is reaped.
  sh_.coproc_name = name;
  sh_.coproc_pid = pid;
  return (sh_.last_status = 0);
}

int Executor::run_subshell(const Subshell *c) {
  // A subshell is a leaf for $BASH_COMMAND too: it is what the ERR trap
  // reports when the subshell fails.
  if (!sh_.in_err_trap) sh_.bash_command = to_string(c);
  pid_t pid = fork();
  if (pid == 0) {
    sh_.job_control = false;  // the subshell runs as one unit; no nested tty control
    sh_.subshell_level++;
    sh_.loop_depth = 0;  // a `break' in a subshell doesn't break the outer loop
    // A subshell does not inherit the parent's EXIT trap; only one it sets for
    // itself runs when it exits.
    sh_.traps.erase("EXIT");
    sh_.traps.erase("CHLD");  // the parent fires CHLD for the subshell as a whole
    sh_.drop_child_traps();
    sh_.pending_sigchld = 0;
    // (external): a lone simple command can exec in place, no second fork.
    if (dynamic_cast<const SimpleCommand *>(c->body.get())) sh_.can_exec_replace = true;
    Executor ex(sh_);
    int s = ex.run(c->body.get());
    sh_.can_exec_replace = false;
    // Run the subshell's own EXIT trap, if it installed one, with $? set to the
    // status of the last command (bash semantics).
    auto it = sh_.traps.find("EXIT");
    if (it != sh_.traps.end()) {
      std::string cmd = it->second;
      sh_.traps.erase(it);
      sh_.last_status = s;
      sh_.exiting = false;
      sh_.run_string(cmd);
      if (sh_.exiting) s = sh_.exit_status;  // the trap ran `exit N'
    }
    std::fflush(nullptr);
    _exit(s & 0xff);
  }
  int wst = 0;
  waitpid(pid, &wst, 0);
  sh_.note_child_reaped();  // a foreground external command that terminated
  return WIFEXITED(wst) ? WEXITSTATUS(wst) : 128;
}

int Executor::run_group(const Group *c) { return run(c->body.get()); }

int Executor::run_if(const IfCommand *c) {
  sh_.errexit_suppress++;
  int cond = run(c->cond.get());
  sh_.errexit_suppress--;
  if (unwinding()) return sh_.last_status;
  if (cond == 0) return run(c->then_part.get());
  if (c->else_part) return run(c->else_part.get());
  return 0;
}

int Executor::run_loop(const LoopCommand *c) {
  int st = 0;
  sh_.loop_depth++;
  while (!unwinding()) {
    sh_.errexit_suppress++;
    int cond = run(c->cond.get());
    sh_.errexit_suppress--;
    bool go = c->until ? (cond != 0) : (cond == 0);
    if (!go) break;
    st = run(c->body.get());
    if (sh_.break_count) { sh_.break_count--; break; }
    // `continue N' with N>1 stops this loop and propagates the remaining count
    // to the enclosing loop; N==1 continues this loop.
    if (sh_.continue_count) { if (--sh_.continue_count) break; else continue; }
  }
  sh_.loop_depth--;
  return st;
}

// bash runs the DEBUG trap for a `for'/`case' command itself, not only for the
// commands inside it -- and for a loop, once per ITERATION, so a trace shows
// the header line each time round.  Returns false when the trap asked to skip
// the command (`return'/`exit' from the body).
bool Executor::debug_trap_for(const Command *c, int line) {
  if (!sh_.traps.count("DEBUG") || sh_.in_debug_trap) return true;
  if (line > 0) sh_.cur_lineno = sh_.lineno_base + line;
  sh_.run_debug_trap(to_string(c));
  return !unwinding();
}

int Executor::run_for(const ForCommand *c) {
  int st = 0;
  if (c->is_select) return run_select(c);
  if (!c->is_arith && !c->var.empty()) {
    // The loop variable must be a valid identifier (validated at execution,
    // as bash does: `NAME: line N: \`x-y': not a valid identifier').
    bool okname = std::isalpha(static_cast<unsigned char>(c->var[0])) || c->var[0] == '_';
    for (char ch : c->var)
      if (!(std::isalnum(static_cast<unsigned char>(ch)) || ch == '_')) okname = false;
    if (!okname) {
      std::fprintf(stderr, "%s`%s': not a valid identifier\n", sh_.err_prefix().c_str(),
                   c->var.c_str());
      // In posix mode a bad iteration-variable name is a fatal error in a
      // non-interactive shell: the shell (or subshell) exits rather than
      // continuing past the loop.
      if (sh_.opt_posix && !sh_.interactive) { sh_.exiting = true; sh_.exit_status = 1; }
      return (sh_.last_status = 1);
    }
  }
  if (c->is_arith) {
    bool ok = true;
    int for_line = sh_.cur_lineno;  // the loop's own line (set by run() on entry)
    // The three arithmetic sections undergo parameter/command expansion before
    // evaluation (as bash does), so forms like ${#arr[@]} work inside them.
    Expander aex(sh_);
    auto aeval = [&](const std::string &e) -> long long {
      if (e.empty()) return 0LL;
      // bash fires the DEBUG trap for each arith-for expression (init, and the
      // test/step on every iteration), reporting the loop's own line, before
      // evaluating it; extdebug lets a non-zero trap skip that evaluation.
      if (sh_.traps.count("DEBUG") && !sh_.in_debug_trap) {
        sh_.cur_lineno = for_line;
        int tst = sh_.run_debug_trap("(( " + e + " ))");
        auto ed = sh_.shopt_opts.find("extdebug");
        if (tst != 0 && ed != sh_.shopt_opts.end() && ed->second) return 0LL;
      }
      if (sh_.opt_xtrace) std::fprintf(sh_.xtrace_out(), "+ (( %s ))\n", e.c_str());
      // Report an arithmetic error with bash's `((: EXPR: ...' diagnostic (the
      // loop-abort above then stops the loop), rather than failing silently.
      return static_cast<long long>(eval_arith_msg(sh_, aex.expand_no_split(e), "((", &ok));
    };
    // An arithmetic error (bad lvalue, division by zero, ...) in any of the
    // three sections aborts the loop with failure status, as bash does; without
    // this a broken step expression like `7++' would spin forever.
    aeval(c->a_init);
    if (!ok) return 1;
    sh_.loop_depth++;
    for (;;) {
      if (!c->a_cond.empty()) {
        long long cv = aeval(c->a_cond);
        if (!ok) { st = 1; break; }
        if (cv == 0) break;
      }
      // No header trap here: an arithmetic `for' already fires one per pass
      // when it evaluates its test expression (see aeval above).
      st = run(c->body.get());
      if (sh_.break_count) { sh_.break_count--; break; }
      // `continue N' with N>1 propagates to the enclosing loop; N==1 runs the
      // update and re-tests, the normal continue path.
      if (sh_.continue_count) { if (--sh_.continue_count) break; }
      if (unwinding()) break;
      aeval(c->a_update);
      if (!ok) { st = 1; break; }
    }
    sh_.loop_depth--;
    return st;
  }

  Expander ex(sh_);
  std::vector<std::string> items;
  if (c->words_present)
    items = ex.expand_args(c->words);
  else
    items = sh_.positional;
  // bash re-emits the `for NAME in WORDS' trace before EVERY iteration (not
  // once for the whole loop), so build it once and print it at the top of each.
  std::string xtrace_line;
  if (sh_.opt_xtrace) {
    xtrace_line = "+ for " + c->var + " in";
    for (const std::string &it : items) xtrace_line += " " + it;
  }
  sh_.loop_depth++;
  for (const std::string &item : items) {
    if (!debug_trap_for(c, c->line)) break;  // the loop header, once per pass
    if (sh_.opt_xtrace) std::fprintf(sh_.xtrace_out(), "%s\n", xtrace_line.c_str());
    // A nameref loop variable is special: each iteration *retargets* the
    // reference to name ITEM rather than writing ITEM through to its current
    // target (bash treats `for ref in a b c' like successive `declare -n
    // ref=a/b/c').  A readonly nameref cannot be retargeted.
    auto vit = sh_.vars.find(c->var);
    if (vit != sh_.vars.end() && vit->second.nameref) {
      if (vit->second.readonly)
        std::fprintf(stderr, "%s%s: readonly variable\n", sh_.err_prefix().c_str(),
                     c->var.c_str());
      else if (!Shell::valid_nameref_target(item)) {
        // Retargeting a nameref to a non-identifier (`for r in /') is an error,
        // like `declare -n r=/'; bash aborts the loop rather than continuing.
        std::fprintf(stderr, "%s`%s': not a valid identifier\n", sh_.err_prefix().c_str(),
                     item.c_str());
        st = 1;
        break;
      } else
        vit->second.value = item;
    } else if (!sh_.set(c->var, item)) {
      // A readonly loop variable aborts the whole loop with status 1, after
      // ONE diagnostic (bash: `for VAR in 1 2 3' with VAR readonly).
      st = 1;
      break;
    }
    st = run(c->body.get());
    if (sh_.break_count) { sh_.break_count--; break; }
    if (sh_.continue_count) { if (--sh_.continue_count) break; else continue; }
    if (unwinding()) break;
  }
  sh_.loop_depth--;
  return st;
}

// `select NAME in WORDS; do BODY; done': print a numbered menu of WORDS to
// stderr, prompt with $PS3 (default `#? '), read a selection into REPLY, bind
// NAME to the chosen word (empty for an out-of-range/invalid choice), and run
// BODY -- looping until EOF or `break'.  Mirrors bash's execute_select_command
// / select_query / print_select_list (execute_cmd.c), including the column
// layout and the KSH_COMPATIBLE reprint-only-after-an-empty-line behavior.
int Executor::run_select(const ForCommand *c) {
  // The loop variable must be a valid identifier, validated at run time as bash
  // does (`NAME: line N: `x-y': not a valid identifier').  The check runs
  // BEFORE the select's own line is installed (bash's execute_select_command
  // calls check_identifier first), so the diagnostic carries the ambient line
  // -- inside a function, the line its body starts on.
  if (!c->var.empty()) {
    bool okname = std::isalpha(static_cast<unsigned char>(c->var[0])) || c->var[0] == '_';
    for (char ch : c->var)
      if (!(std::isalnum(static_cast<unsigned char>(ch)) || ch == '_')) okname = false;
    if (!okname) {
      std::fprintf(stderr, "%s`%s': not a valid identifier\n", sh_.err_prefix().c_str(),
                   c->var.c_str());
      if (sh_.opt_posix && !sh_.interactive) { sh_.exiting = true; sh_.exit_status = 1; }
      return (sh_.last_status = 1);
    }
  }
  if (c->line > 0) sh_.cur_lineno = sh_.lineno_base + c->line;  // $LINENO / errors
  Expander ex(sh_);
  std::vector<std::string> items =
      c->words_present ? ex.expand_args(c->words) : sh_.positional;
  if (items.empty()) return (sh_.last_status = 0);  // empty list: no loop (bash)
  const int n = static_cast<int>(items.size());

  // Character-count display width (bash uses wcswidth; byte count in C locale).
  auto dwidth = [](const std::string &s) -> int {
    if (MB_CUR_MAX <= 1) return static_cast<int>(s.size());
    int w = 0;
    size_t i = 0;
    std::mbstate_t st{};
    while (i < s.size()) {
      size_t r = std::mbrtowc(nullptr, s.data() + i, s.size() - i, &st);
      if (r == static_cast<size_t>(-2)) break;
      if (r == static_cast<size_t>(-1) || r == 0) { r = 1; st = std::mbstate_t{}; }
      i += r;
      w++;
    }
    return w;
  };
  auto numlen = [](int v) -> int {
    int d = 1;
    for (int x = v; x >= 10; x /= 10) d++;
    return d;
  };

  // Menu geometry (print_select_list).  RP_SPACE is ") ", RP_SPACE_LEN 2.
  int maxw = 0;
  for (const auto &s : items) maxw = std::max(maxw, dwidth(s));
  const int indices_len = numlen(n);
  const int max_elem_len = maxw + indices_len + 2 /*") "*/ + 2;
  std::string colstr = sh_.get("COLUMNS");
  int cols_avail = 80;
  if (!colstr.empty()) { int cv = std::atoi(colstr.c_str()); if (cv > 0) cols_avail = cv; }

  auto print_menu = [&]() {
    int cols = max_elem_len ? cols_avail / max_elem_len : 1;
    if (cols == 0) cols = 1;
    int rows = n / cols + (n % cols != 0);
    cols = n / rows + (n % rows != 0);
    if (rows == 1) { rows = cols; cols = 1; }
    int first_idxlen = numlen(rows);
    for (int row = 0; row < rows; row++) {
      int ind = row, pos = 0;
      for (;;) {
        int idxlen = (pos == 0) ? first_idxlen : indices_len;
        std::fprintf(stderr, "%*d) %s", idxlen, ind + 1, items[ind].c_str());
        int elem_len = dwidth(items[ind]) + idxlen + 2;
        ind += rows;
        if (ind >= n) break;
        // indent from (pos+elem_len) to (pos+max_elem_len) with tabs/spaces.
        int from = pos + elem_len, to = pos + max_elem_len, tab = 8;
        while (from < to) {
          if (to / tab > from / tab) { std::fputc('\t', stderr); from += tab - from % tab; }
          else { std::fputc(' ', stderr); from++; }
        }
        pos += max_elem_len;
      }
      std::fputc('\n', stderr);
    }
  };

  auto read_line = [](std::string &out) -> bool {
    out.clear();
    bool any = false;
    char ch;
    for (;;) {
      ssize_t r = ::read(0, &ch, 1);
      if (r <= 0) return any;  // EOF: a partial final line still counts as read
      any = true;
      if (ch == '\n') return true;
      out += ch;
    }
  };

  int st = 0;
  bool show_menu = true;
  sh_.loop_depth++;
  for (;;) {
    // select_query: (re)print the menu when needed, prompt, and read a choice.
    std::string selection;
    bool eof = false, have_sel = false;
    for (;;) {
      if (show_menu) print_menu();
      std::string ps3 = sh_.is_set("PS3") ? sh_.get("PS3") : "#? ";
      std::fprintf(stderr, "%s", ps3.c_str());
      std::fflush(stderr);
      std::string line;
      if (!read_line(line)) { eof = true; break; }
      sh_.set("REPLY", line);
      if (line.empty()) { show_menu = true; continue; }  // blank line reprints
      // Parse a decimal index, tolerating surrounding whitespace (valid_number).
      const char *p = line.c_str();
      while (std::isspace(static_cast<unsigned char>(*p))) p++;
      char *end = nullptr;
      long v = std::strtol(p, &end, 10);
      bool numeric = end != p;
      if (numeric) { while (std::isspace(static_cast<unsigned char>(*end))) end++; }
      selection = (numeric && *end == '\0' && v >= 1 && v <= n) ? items[v - 1]
                                                                : std::string();
      have_sel = true;
      break;
    }
    if (eof) { std::fputc('\n', stdout); std::fflush(stdout); st = 1; break; }
    if (!have_sel) break;
    // Bind NAME (write-through a nameref, honoring readonly); a failed bind
    // aborts the select without running the body, as bash does.
    if (!sh_.set(c->var, selection)) { st = 1; break; }
    st = run(c->body.get());
    if (sh_.break_count) { sh_.break_count--; break; }
    if (sh_.continue_count) { if (--sh_.continue_count) break; }
    if (unwinding()) break;
    show_menu = false;  // KSH_COMPATIBLE: don't reprint the menu after a choice
  }
  sh_.loop_depth--;
  return (sh_.last_status = st);
}

int Executor::run_case(const CaseCommand *c) {
  if (!debug_trap_for(c, c->line)) return sh_.last_status;
  Expander ex(sh_);
  std::string word = ex.expand_no_split(c->word.text);
  if (sh_.arith_error) { sh_.arith_error = false; return (sh_.last_status = 1); }
  if (sh_.opt_xtrace) std::fprintf(sh_.xtrace_out(), "+ case %s in\n", word.c_str());
  int st = 0;
  size_t i = 0;
  while (i < c->clauses.size()) {
    const CaseClause &cl = c->clauses[i];
    bool m = false;
    for (const Word &pat : cl.patterns) {
      std::string p = ex.expand_pattern(pat.text);
      if (sh_.arith_error) { sh_.arith_error = false; return (sh_.last_status = 1); }
      std::string pp = p, ww = word;
      if (strmatch(pp.data(), ww.data(), FNM_EXTMATCH) == 0) {
        m = true;
        break;
      }
    }
    if (!m) {
      i++;
      continue;
    }
    st = cl.body ? run(cl.body.get()) : 0;
    if (unwinding()) return st;
    // `;&' falls through into the following clause's body (and keeps falling
    // while those clauses also end in `;&').
    while (c->clauses[i].terminator == 1 && i + 1 < c->clauses.size()) {
      i++;
      st = c->clauses[i].body ? run(c->clauses[i].body.get()) : 0;
      if (unwinding()) return st;
    }
    if (c->clauses[i].terminator == 2) {  // `;;&': resume testing patterns
      i++;
      continue;
    }
    return st;
  }
  return st;
}

// The relative source line of the first executable command in a body, found by
// descending through wrapping groups/connections; 0 if none carries a line.
static int first_body_line(const Command *c) {
  if (!c) return 0;
  if (auto *g = dynamic_cast<const Group *>(c)) return first_body_line(g->body.get());
  if (auto *s = dynamic_cast<const Subshell *>(c)) return first_body_line(s->body.get());
  if (auto *cn = dynamic_cast<const Connection *>(c)) {
    int f = first_body_line(cn->first.get());
    return f > 0 ? f : first_body_line(cn->second.get());
  }
  return c->line;
}

int Executor::run_funcdef(const FunctionDef *c) {
  // bash rejects a function name that contained an unquoted `$' expansion
  // (W_HASDOLLAR) -- e.g. `function sys$read' -- or any quoting (`'a b c' ()')
  // as not a valid identifier; the raw word, quotes included, is echoed.
  if (c->name.find_first_of("$'\"\\") != std::string::npos ||
      // A process-substitution-shaped name (`<(:) ()') is likewise rejected.
      ((c->name[0] == '<' || c->name[0] == '>') && c->name.size() > 1 && c->name[1] == '(')) {
    std::fprintf(stderr, "%s`%s': not a valid identifier\n",
                 sh_.err_prefix().c_str(), c->name.c_str());
    return (sh_.last_status = 1);
  }
  // Posix interpretation 383: a function may not have the name of a special
  // builtin.  bash reports it and aborts a non-interactive shell outright
  // (jump_to_top_level(ERREXIT) with EX_BADUSAGE).
  if (sh_.opt_posix) {
    static const std::set<std::string> kSpecialB = {
        ":",      ".",     "break", "continue", "eval",  "exec",
        "exit",   "export", "readonly", "return", "set", "shift",
        "source", "times", "trap",  "unset"};
    if (kSpecialB.count(c->name)) {
      std::fprintf(stderr, "%s`%s': is a special builtin\n", sh_.err_prefix().c_str(),
                   c->name.c_str());
      sh_.last_status = 2;
      if (!sh_.interactive) { sh_.exiting = true; sh_.exit_status = 2; }
      return 2;
    }
  }
  // A readonly function cannot be redefined.
  if (sh_.readonly_functions.count(c->name)) {
    std::fprintf(stderr, "%s%s: readonly function\n", sh_.err_prefix().c_str(),
                 c->name.c_str());
    return (sh_.last_status = 1);
  }
  sh_.functions[c->name] = c->body.get();
  sh_.func_src[c->name] = sh_.current_source();  // file it was defined in, for BASH_SOURCE
  sh_.func_lineno_base[c->name] = sh_.lineno_base;  // for $LINENO inside the body
  // The `name ()' line = one above the body's first command (both the entry
  // DEBUG trap and `caller' report it); computed absolutely under the body base.
  int fbl = first_body_line(c->body.get());
  sh_.func_def_line[c->name] = fbl > 0 ? sh_.lineno_base + fbl - 1 : sh_.cur_lineno;
  return 0;
}

int Executor::run_cond(const CondCommand *c) {
  // A conditional is a leaf command, not a wrapper: like a simple command it
  // sets $BASH_COMMAND and, failing, fires the ERR trap.
  sh_.bash_command = to_string(c);
  // Minimal [[ ]] evaluation delegated to the test builtin semantics via a
  // small dispatch here.  For now, evaluate simple `a OP b`, unary, and !/&&/||
  // by reusing the expression string tokens is complex; approximate with the
  // test builtin over expanded tokens.
  extern bool eval_cond_expression(Shell &, const std::string &, int *);
  int st = 1;
  eval_cond_expression(sh_, c->expression, &st);
  return st;
}

int Executor::run_arith(const ArithCommand *c) {
  sh_.bash_command = to_string(c);  // a leaf command, as `[[ ]]' is
  if (!debug_trap_for(c, c->line)) return sh_.last_status;
  bool ok = true;
  Expander ex(sh_);  // expand ${#arr[@]} etc. before arithmetic evaluation
  std::string e = ex.expand_arith(c->expression);
  // xtrace prints the POST-EXPANSION expression (`(( $var ))' traces as
  // `+ ((  42  ))'), as bash does.
  if (sh_.opt_xtrace) std::fprintf(sh_.xtrace_out(), "+ (( %s ))\n", e.c_str());
  // The `(( ))' command reports an arithmetic error (bad token, division by
  // zero) with bash's `((: EXPR: ...' diagnostic, unlike bare $(( )).
  long long v = eval_arith_msg(sh_, e, "((", &ok, /*expand_subs=*/1);
  if (!ok) return 1;
  return v != 0 ? 0 : 1;
}

}  // namespace gnash::core
