// Copyright (c) 2026 Brian J. Fox
// Licensed under GPLv2 with the GPLv2-AI Exception.

// readline.cpp -- the interactive loop: key dispatch, redisplay, numeric
// arguments, history movement, and the readline() entry point.
//
// Structure follows bash 5.3 lib/readline/readline.c at a high level: read a
// key, dispatch it through the current keymap (following ISKMAP prefixes for
// ESC/meta and CSI/SS3 sequences), redisplay, and loop until a command sets
// rl_done.  Redisplay is a full single-line repaint using termcap's
// clear-to-end-of-line; multi-row wrapping is a later refinement.

#include <cctype>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#include "gnash/readline_internal.hpp"
#include "gnash/sh/xmalloc.hpp"
#include "readline/history.h"
#include "readline/readline.h"
#include "termcap.h"

using gnash::readline::build_emacs_keymaps;
using gnash::readline::build_vi_keymaps;
using gnash::readline::deprep_terminal;
using gnash::readline::maybe_init_line;
using gnash::readline::prep_terminal;

// ---- exported state -------------------------------------------------------
extern "C" {
const char *rl_prompt = nullptr;
FILE *rl_instream = nullptr;
FILE *rl_outstream = nullptr;
int rl_numeric_arg = 1;
int rl_explicit_arg = 0;
int rl_eof_found = 0;
rl_command_func_t *rl_last_func = nullptr;  // the command dispatched last
rl_hook_func_t *rl_event_hook = nullptr;
// Optional syntax-highlighting hook: fills colors[len] with a color id per
// character (0=none, 1=green, 2=red, 3=yellow, 4=cyan).  NULL disables it.
void (*rl_highlight_function)(const char *line, int len, int *colors) = nullptr;
// Nonzero after readline() returned because a SIGINT (C-c) aborted the line.
int rl_pending_sigint = 0;
}

// Shared with input.cpp so the key-read loop bails out of a blocked read().
namespace gnash::readline {
volatile std::sig_atomic_t rl_sigint_flag = 0;
}

namespace {

// SIGINT handler installed for the duration of readline() on a tty.  Just
// records the signal; the read loop notices the flag and aborts the line
// (doing display/terminal cleanup outside async-signal context).
void rl_sigint_handler(int) { gnash::readline::rl_sigint_flag = 1; }

int arg_sign = 1;
std::string saved_line;  // the typed line, stashed while browsing history

// operate-and-get-next: 1-based history index to preload at the next
// readline() call, or -1.
int saved_history_offset = -1;

// Replace the visible line and put the cursor at the end.  Loading a
// different line discards the undo history (history motion is not undoable).
void put_line(const char *text) {
  gnash::readline::undo_clear();
  rl_replace_line(text ? text : "", 1);
  rl_point = rl_end;
}

// Cached clear-to-end-of-line capability.
const char *clear_eol() {
  static std::string ce;
  static bool init = false;
  if (!init) {
    init = true;
    const char *term = std::getenv("TERM");
    if (term && tgetent(nullptr, term) == 1) {
      char *s = tgetstr("ce", nullptr);
      if (s) ce = s;
    }
    if (ce.empty()) ce = "\033[K";  // ANSI fallback
  }
  return ce.c_str();
}

// Termcap "cl" (clear screen + home cursor), with an ANSI fallback.
const char *clear_screen_seq() {
  static std::string cl;
  static bool init = false;
  if (!init) {
    init = true;
    const char *term = std::getenv("TERM");
    if (term && tgetent(nullptr, term) == 1) {
      char *s = tgetstr("cl", nullptr);
      if (s) cl = s;
    }
    if (cl.empty()) cl = "\033[H\033[2J";  // ANSI: home + clear entire screen
  }
  return cl.c_str();
}

int dispatch(int key, Keymap map);

// Accumulate a numeric argument, then dispatch the terminating key with it.
int digit_loop() {
  for (;;) {
    int c = rl_read_key();
    if (c == EOF) return 0;
    if (c >= '0' && c <= '9') {
      if (rl_explicit_arg)
        rl_numeric_arg = rl_numeric_arg * 10 + (c - '0');
      else {
        rl_numeric_arg = c - '0';
        rl_explicit_arg = 1;
      }
    } else if (c == '-') {
      arg_sign = -arg_sign;
    } else {
      rl_numeric_arg *= arg_sign;
      return dispatch(c, rl_get_keymap());
    }
  }
}

int dispatch(int key, Keymap map) {
  if (key < 0 || key >= KEYMAP_SIZE) return -1;
  KEYMAP_ENTRY e = map[key];

  switch (e.type) {
    case ISKMAP: {
      Keymap sub = static_cast<Keymap>(e.kmap);
      if (sub == nullptr) return rl_ding();

      // vi mode's ESC prefix: a lone ESC in insert mode enters command mode;
      // ESC followed by a key that isn't part of an escape sequence enters
      // command mode and runs the key there (bash's keyseq-timeout behavior).
      if (key == 0x1b &&
          (map == vi_insertion_keymap || map == vi_movement_keymap)) {
        bool inserting = map == vi_insertion_keymap;
        if (!gnash::readline::input_pending(250))
          return inserting ? rl_vi_movement_mode(1, key) : rl_ding();
        int k2 = rl_read_key();
        if (k2 == EOF) return -1;
        KEYMAP_ENTRY e2 = (k2 >= 0 && k2 < KEYMAP_SIZE)
                              ? sub[k2]
                              : KEYMAP_ENTRY{ISFUNC, nullptr, nullptr};
        if (e2.type == ISKMAP ? e2.kmap != nullptr : e2.function != nullptr)
          return dispatch(k2, sub);
        if (inserting) rl_vi_movement_mode(1, key);
        return dispatch(k2, rl_get_keymap());
      }

      int k2 = rl_read_key();
      if (k2 == EOF) return -1;
      return dispatch(k2, sub);
    }
    case ISFUNC:
    default:
      if (e.function) {
        // Meta/C-x uppercase letters run the lowercase binding of the same map.
        if (e.function == rl_do_lowercase_version)
          return dispatch(std::tolower(key), map);

        int count = rl_explicit_arg ? rl_numeric_arg : 1;

        // vi `.': capture the keys of a change command (including the keys it
        // reads itself, and everything typed until command mode resumes).
        if (!gnash::readline::redo_capturing && map == vi_movement_keymap &&
            gnash::readline::vi_change_starter(e.function)) {
          gnash::readline::redo_capture.clear();
          if (rl_explicit_arg) gnash::readline::redo_capture += std::to_string(count);
          gnash::readline::redo_capture += static_cast<char>(key);
          gnash::readline::redo_capturing = true;
        }

        // Undo: snapshot the line, and record it if the command changed the
        // buffer.  Skipped for the undo commands themselves, for the numeric-
        // argument readers (the command they dispatch records itself), and
        // when the command replaced the line from history (that clears undo
        // state and must not itself become undoable).
        std::string before(rl_line_buffer, static_cast<size_t>(rl_end));
        int before_point = rl_point;
        unsigned gen = gnash::readline::undo_generation();

        int r = e.function(count, key);

        bool undoable = e.function != rl_undo_command &&
                        e.function != rl_revert_line && e.function != rl_vi_undo &&
                        e.function != rl_digit_argument &&
                        e.function != rl_universal_argument;
        if (undoable && gen == gnash::readline::undo_generation() &&
            (static_cast<int>(before.size()) != rl_end ||
             std::memcmp(before.data(), rl_line_buffer, before.size()) != 0))
          gnash::readline::undo_push(before, before_point);

        // A change command is fully captured once command mode is active again
        // (immediately for x/r/d..., after the closing ESC for i/a/c...).
        if (gnash::readline::redo_capturing &&
            rl_get_keymap() == vi_movement_keymap) {
          gnash::readline::redo_capturing = false;
          gnash::readline::redo_keys = gnash::readline::redo_capture;
        }

        rl_last_func = e.function;  // so a command can tell it ran twice in a row
        rl_numeric_arg = 1;
        rl_explicit_arg = 0;
        arg_sign = 1;
        return r;
      }
      return rl_ding();
  }
}

}  // namespace

// ---- bindable commands specific to the interactive layer ------------------

extern "C" int rl_newline(int /*count*/, int /*key*/) {
  rl_done = 1;
  return 0;
}

extern "C" int rl_eof_or_delete(int count, int key) {
  if (rl_end == 0) {
    rl_eof_found = 1;
    rl_done = 1;
    return 0;
  }
  return rl_delete(count, key);
}

extern "C" int rl_get_previous_history(int count, int key) {
  if (count < 0) return rl_get_next_history(-count, key);
  if (count == 0) return 0;
  if (where_history() == history_length)
    saved_line.assign(rl_line_buffer, static_cast<size_t>(rl_end));
  HIST_ENTRY *e = nullptr;
  while (count-- > 0) {
    HIST_ENTRY *t = previous_history();
    if (t == nullptr) break;
    e = t;
  }
  if (e == nullptr) return rl_ding();
  put_line(e->line);
  return 0;
}

extern "C" int rl_get_next_history(int count, int key) {
  if (count < 0) return rl_get_previous_history(-count, key);
  if (count == 0) return 0;
  HIST_ENTRY *e = nullptr;
  while (count-- > 0) {
    HIST_ENTRY *t = next_history();
    if (t == nullptr) break;
    e = t;
  }
  if (e)
    put_line(e->line);
  else
    put_line(saved_line.c_str());
  return 0;
}

extern "C" int rl_beginning_of_history(int /*count*/, int key) {
  return rl_get_previous_history(1 + where_history(), key);
}

extern "C" int rl_end_of_history(int /*count*/, int /*key*/) {
  while (next_history() != nullptr) {}
  put_line(saved_line.c_str());
  return 0;
}

// Fetch history entry COUNT (numbered as `history' prints them) with an
// explicit argument, else the first entry.  Bound to vi `G'.
extern "C" int rl_fetch_history(int count, int key) {
  if (rl_explicit_arg) {
    int nhist = history_base + where_history();
    int wanted = count >= 0 ? nhist - count : -count;
    if (wanted <= 0 || wanted >= nhist) {
      if (rl_editing_mode == 0) return rl_ding();  // vi: out of range, no move
      return rl_beginning_of_history(0, key);
    }
    return rl_get_previous_history(wanted, key);
  }
  return rl_beginning_of_history(0, key);
}

// C-o: accept this line and preload the next history entry at the next prompt.
extern "C" int rl_operate_and_get_next(int count, int key) {
  rl_newline(1, key);
  saved_history_offset = rl_explicit_arg ? count : where_history() + history_base + 1;
  return 0;
}

extern "C" int rl_digit_argument(int /*count*/, int key) {
  arg_sign = 1;
  if (key == '-') {
    arg_sign = -1;
    rl_explicit_arg = 0;
    rl_numeric_arg = 1;
  } else {
    rl_numeric_arg = key - '0';
    rl_explicit_arg = 1;
  }
  return digit_loop();
}

extern "C" int rl_universal_argument(int /*count*/, int /*key*/) {
  rl_numeric_arg = rl_explicit_arg ? rl_numeric_arg * 4 : 4;
  rl_explicit_arg = 1;
  arg_sign = 1;
  return digit_loop();
}

// ---- redisplay ------------------------------------------------------------

namespace {

int screen_width() {
  struct winsize ws;
  if (rl_outstream && ioctl(fileno(rl_outstream), TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
    return ws.ws_col;
  const char *c = std::getenv("COLUMNS");
  if (c) {
    int v = std::atoi(c);
    if (v > 0) return v;
  }
  return 80;
}

}  // namespace

// The terminal width in columns (from TIOCGWINSZ, else $COLUMNS, else 80).
extern "C" int rl_get_screen_width(void) { return screen_width(); }

// Single-row redisplay with horizontal scrolling: the line stays on one screen
// line, and the visible window slides to keep the cursor in view.  (Multi-row
// wrapped redisplay is a later refinement.)
extern "C" void rl_redisplay(void) {
  if (rl_outstream == nullptr) rl_outstream = stdout;
  FILE *o = rl_outstream;

  int width = screen_width();
  int plen = rl_prompt ? static_cast<int>(std::strlen(rl_prompt)) : 0;
  int avail = width - plen - 1;
  if (avail < 1) avail = 1;

  int off = (rl_point > avail) ? rl_point - avail : 0;
  int shown = rl_end - off;
  if (shown > avail) shown = avail;

  std::fputc('\r', o);
  if (rl_prompt) std::fputs(rl_prompt, o);
  if (shown > 0) {
    if (rl_highlight_function && rl_end > 0) {
      // Emit per-character color (zero-width ANSI codes, so the column math is
      // unchanged) for the visible window.
      static const char *const kAnsi[] = {"\033[0m", "\033[32m", "\033[31m",
                                          "\033[33m", "\033[36m"};
      std::vector<int> col(static_cast<size_t>(rl_end), 0);
      rl_highlight_function(rl_line_buffer, rl_end, col.data());
      int cur = 0;
      for (int k = off; k < off + shown; k++) {
        int c = (k < rl_end && col[k] >= 0 && col[k] < 5) ? col[k] : 0;
        if (c != cur) { std::fputs(kAnsi[c], o); cur = c; }
        std::fputc(rl_line_buffer[k], o);
      }
      if (cur != 0) std::fputs("\033[0m", o);
    } else {
      std::fwrite(rl_line_buffer + off, 1, static_cast<size_t>(shown), o);
    }
  }
  std::fputs(clear_eol(), o);

  int endcol = plen + shown;
  int curcol = plen + (rl_point - off);
  for (int i = endcol; i > curcol; i--) std::fputc('\b', o);
  std::fflush(o);
}

// C-l: clear the terminal and redraw the prompt with the current input line at
// the top of the screen (readline's `clear-screen').
extern "C" int rl_clear_screen(int /*count*/, int /*key*/) {
  if (rl_outstream == nullptr) rl_outstream = stdout;
  std::fputs(clear_screen_seq(), rl_outstream);
  rl_redisplay();  // repaint prompt + buffer at row 0
  return 0;
}

// Erase the current input line so a caller (e.g. an event hook printing a
// background-job notice) can write where the prompt was, then rl_redisplay().
extern "C" void rl_clear_current_line(void) {
  if (rl_outstream == nullptr) rl_outstream = stdout;
  std::fputc('\r', rl_outstream);
  std::fputs(clear_eol(), rl_outstream);
  std::fflush(rl_outstream);
}

// ---- top level ------------------------------------------------------------

extern "C" int rl_initialize(void) {
  build_emacs_keymaps();
  build_vi_keymaps();
  maybe_init_line();
  return 0;
}

extern "C" char *readline(const char *prompt) {
  rl_initialize();
  if (rl_instream == nullptr) rl_instream = stdin;
  if (rl_outstream == nullptr) rl_outstream = stdout;
  rl_prompt = prompt ? prompt : "";

  maybe_init_line();
  rl_point = rl_end = 0;
  rl_mark = 0;
  rl_line_buffer[0] = '\0';
  rl_done = 0;
  rl_eof_found = 0;
  rl_numeric_arg = 1;
  rl_explicit_arg = 0;
  arg_sign = 1;

  using_history();
  saved_line.clear();
  gnash::readline::undo_clear();
  gnash::readline::macro_recording = false;
  gnash::readline::redo_capturing = false;

  // operate-and-get-next (C-o) accepted the previous line: preload the history
  // entry after it.
  if (saved_history_offset >= 0) {
    int back = where_history() - (saved_history_offset - history_base);
    saved_history_offset = -1;
    if (back > 0) rl_get_previous_history(back, 0);
  }

  rl_last_func = nullptr;  // a new line: nothing was dispatched before it
  // Start each read in the base keymap for the active editing mode.
  rl_set_keymap(rl_editing_mode == 0 ? vi_insertion_keymap
                                     : rl_get_keymap_by_name("emacs"));

  int fd = fileno(rl_instream);
  bool tty = isatty(fd) != 0;
  rl_pending_sigint = 0;
  gnash::readline::rl_sigint_flag = 0;
  // On a tty, catch SIGINT ourselves so C-c aborts the current line and
  // reprompts (the shell otherwise ignores SIGINT while at the prompt).  No
  // SA_RESTART, so a blocked read()/select() returns EINTR and the loop can
  // notice the flag.  The previous disposition is restored on return.
  struct sigaction old_int;
  bool int_installed = false;
  if (tty) {
    struct sigaction sa;
    std::memset(&sa, 0, sizeof sa);
    sa.sa_handler = rl_sigint_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    if (sigaction(SIGINT, &sa, &old_int) == 0) int_installed = true;
    prep_terminal(fd);
    rl_redisplay();
  }

  while (!rl_done) {
    int c = rl_read_key();
    // Erase a completion listing left below the line by a prior TAB (the cursor
    // is still on the input line at this point), before anything else reacts to
    // the key -- including a C-c/EOF that ends the line.
    rl_clear_menu_below();
    if (gnash::readline::rl_sigint_flag) {  // C-c: abort this line, reprompt
      gnash::readline::rl_sigint_flag = 0;
      rl_pending_sigint = 1;
      if (tty) { std::fputc('^', rl_outstream); std::fputc('C', rl_outstream); }
      rl_line_buffer[0] = '\0';
      rl_point = rl_end = 0;
      break;
    }
    if (c == EOF) {
      if (rl_end == 0) rl_eof_found = 1;
      break;
    }
    dispatch(c, rl_get_keymap());
    if (tty && !rl_done) rl_redisplay();
  }

  if (tty) {
    deprep_terminal(fd);
    std::fputc('\n', rl_outstream);
    std::fflush(rl_outstream);
    if (int_installed) sigaction(SIGINT, &old_int, nullptr);
  }

  if (rl_pending_sigint) return gnash::sh::savestring("");  // discarded line
  if (rl_eof_found && rl_end == 0) return nullptr;
  return gnash::sh::savestring(rl_line_buffer);
}
