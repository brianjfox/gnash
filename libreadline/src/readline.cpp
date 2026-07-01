// readline.cpp -- the interactive loop: key dispatch, redisplay, numeric
// arguments, history movement, and the readline() entry point.
//
// Structure follows bash 5.3 lib/readline/readline.c at a high level: read a
// key, dispatch it through the current keymap (following ISKMAP prefixes for
// ESC/meta and CSI/SS3 sequences), redisplay, and loop until a command sets
// rl_done.  Redisplay is a full single-line repaint using termcap's
// clear-to-end-of-line; multi-row wrapping is a later refinement.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
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
rl_hook_func_t *rl_event_hook = nullptr;
}

namespace {

int arg_sign = 1;
std::string saved_line;  // the typed line, stashed while browsing history

// Replace the visible line and put the cursor at the end.
void put_line(const char *text) {
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
      int k2 = rl_read_key();
      if (k2 == EOF) return -1;
      return dispatch(k2, sub);
    }
    case ISFUNC:
    default:
      if (e.function) {
        int count = rl_explicit_arg ? rl_numeric_arg : 1;
        int r = e.function(count, key);
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

extern "C" int rl_get_previous_history(int /*count*/, int /*key*/) {
  if (where_history() == history_length)
    saved_line.assign(rl_line_buffer, static_cast<size_t>(rl_end));
  HIST_ENTRY *e = previous_history();
  if (e == nullptr) return rl_ding();
  put_line(e->line);
  return 0;
}

extern "C" int rl_get_next_history(int /*count*/, int /*key*/) {
  HIST_ENTRY *e = next_history();
  if (e)
    put_line(e->line);
  else
    put_line(saved_line.c_str());
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
  if (shown > 0) std::fwrite(rl_line_buffer + off, 1, static_cast<size_t>(shown), o);
  std::fputs(clear_eol(), o);

  int endcol = plen + shown;
  int curcol = plen + (rl_point - off);
  for (int i = endcol; i > curcol; i--) std::fputc('\b', o);
  std::fflush(o);
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

  // Start each read in the base keymap for the active editing mode.
  rl_set_keymap(rl_editing_mode == 0 ? vi_insertion_keymap
                                     : rl_get_keymap_by_name("emacs"));

  int fd = fileno(rl_instream);
  bool tty = isatty(fd) != 0;
  if (tty) {
    prep_terminal(fd);
    rl_redisplay();
  }

  while (!rl_done) {
    int c = rl_read_key();
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
  }

  if (rl_eof_found && rl_end == 0) return nullptr;
  return gnash::sh::savestring(rl_line_buffer);
}
