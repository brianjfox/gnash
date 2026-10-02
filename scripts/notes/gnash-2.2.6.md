# gnash 2.2.6

A patch release: the terminal is now also put back in order when the
killed program ran inside a subshell, subshells and command
substitutions report the signal that killed them, and options after
`-c` are parsed as bash parses them.

## Job control

- The 2.2.5 fix restored the terminal after a foreground program was
  killed by a signal, but only for simple commands, pipelines and `fg`.
  A program run in a subshell -- `( cd proj && bun run dev )` then C-c,
  or `( prog )` where `prog` dies in raw mode -- still left the next
  prompt without echo or line editing. bash restores the terminal there
  because the subshell process itself dies by the signal (it execs its
  last command in place, and re-raises a keyboard SIGINT that killed its
  child). gnash now does the equivalent: a subshell whose last foreground
  command was killed by a signal dies by that signal, the parent restores
  the saved terminal settings, and `$?` is `128 + signal` (130 for
  SIGINT) instead of a flat 128. As in bash, a subshell whose killed
  command was not the last one -- `( prog; true )` -- exits normally and
  the terminal stays as the program left it (#709).
- A command substitution whose command is killed by a signal reports
  `128 + signal`: `x=$(sh -c 'kill -TERM $$'); echo $?` prints 143, as
  in bash, rather than 128 (#709).

## Invocation

- Options that follow `-c` are parsed as options: `gnash -c -l 'echo ok'`
  runs `echo ok` as a login shell instead of taking `-l` as the command
  string and `echo ok` as `$0`. Option parsing runs on to the first
  non-option word, which is the command string, so `-c -e CMD`, `-c -o
  posix CMD` and `-c -- CMD` all behave as in bash. A new differential
  test, `invocation_test`, checks argv handling at invocation against
  bash (#704, contributed by Abhimanyu Aryan).

## Install

```
brew tap brianjfox/tools && brew trust brianjfox/tools && brew install gnash
```

Or download the macOS tarball for your architecture (arm64 for
Apple Silicon, x86_64 for Intel) below.
