# gnash 2.2.5

A patch release with one fix: the terminal is put back in order after a
foreground program is killed by a signal.

## Job control

- A program that switched the terminal to raw mode (`vim`, `less`, a
  curses tool) and was then killed by C-c left the terminal raw for the
  next prompt: no echo, no line editing. gnash never saved the shell's
  terminal settings and never restored them after a foreground job. It
  now does what bash's `wait_for` does: an interactive shell records the
  terminal state at job-control startup and after every foreground job
  that exits normally, so deliberate changes such as `stty -echo`
  persist, and restores that recorded state when a foreground job is
  killed or stopped by a signal. Simple commands, pipelines (a signaled
  stage anywhere counts, as in bash's `job_signal_status`) and `fg` are
  all covered. While readline has the terminal prepped, as for a command
  run from programmable completion, the settings are not recorded (#707).

## Install

```
brew tap brianjfox/tools && brew trust brianjfox/tools && brew install gnash
```

Or download the macOS tarball for your architecture (arm64 for
Apple Silicon, x86_64 for Intel) below.
