# gnash 2.2.7

A patch release: the prompt recovers when a program that outlived the
last job resets the terminal from behind readline's back.

## Line editing

- After C-c on a dev-server orchestrator such as `bun run dev`, the next
  prompt could appear dead until Return was pressed: every key was
  echoed by the kernel and buffered, and C-c, C-e and the rest never
  reached readline. C-c kills the orchestrator at once, so the shell
  takes the terminal back and readline sets up its cbreak/no-echo mode;
  but a grandchild doing a graceful shutdown is still alive in the
  orphaned job group, still holding the tty it had been reading, and
  when it exits a moment later it writes back the cooked settings it
  saved at startup (as bun and node do). bash shows the same symptom.
  gnash's readline now remembers the modes it set and, on each idle tick
  of its key-wait loop (every 100ms), puts them back if something changed
  them while the shell still owns the terminal. Recovery takes at most
  one tick; `stty` changes made at the prompt are untouched, since each
  prompt's modes start from the terminal's current settings (#712).

## Install

```
brew tap brianjfox/tools && brew trust brianjfox/tools && brew install gnash
```

Or download the macOS tarball for your architecture (arm64 for
Apple Silicon, x86_64 for Intel) below.
