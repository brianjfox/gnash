// tty.cpp -- terminal raw-mode setup and restore (rltty analogue).
//
// Puts the terminal into cbreak/no-echo so readline sees keystrokes as they
// are typed, while leaving signal generation (ISIG) on so C-c still works.
// No-op on non-ttys, so readline can be driven from a pipe in tests.

#include <termios.h>
#include <unistd.h>

#include "gnash/readline_internal.hpp"

namespace gnash::readline {

namespace {
struct termios saved_tio;
bool tio_saved = false;
}  // namespace

void prep_terminal(int fd) {
  if (!isatty(fd)) return;
  if (tcgetattr(fd, &saved_tio) != 0) return;
  tio_saved = true;

  struct termios t = saved_tio;
  t.c_lflag &= static_cast<tcflag_t>(~(ICANON | ECHO));
  t.c_iflag &= static_cast<tcflag_t>(~(ICRNL | INLCR));
  t.c_cc[VMIN] = 1;
  t.c_cc[VTIME] = 0;
  tcsetattr(fd, TCSADRAIN, &t);
}

void deprep_terminal(int fd) {
  if (!tio_saved) return;
  tcsetattr(fd, TCSADRAIN, &saved_tio);
  tio_saved = false;
}

}  // namespace gnash::readline
