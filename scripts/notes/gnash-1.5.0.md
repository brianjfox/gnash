# gnash 1.5.0

The headline of this release is a working **`coproc`** command.

## Coprocesses

- `coproc [NAME] command` now runs the command asynchronously with its stdin and
  stdout connected to pipes, and publishes the descriptors and pid the shell uses
  to talk to it:
  - `NAME[0]` — the descriptor the shell reads from (the coprocess's stdout),
  - `NAME[1]` — the descriptor the shell writes to (the coprocess's stdin),
  - `NAME_PID` — the coprocess's pid (`NAME` defaults to `COPROC`).
- The coprocess is a background job, so `wait $NAME_PID` works and its exit
  status is reported. The shell's descriptors are kept on high, close-on-exec
  numbers so they do not collide with user redirections or leak into external
  commands.

  Previously `coproc` parsed but did nothing — the body never ran and none of the
  variables were set, so a coprocess could not be communicated with. Bidirectional
  read/write, `wait`, and status now match bash 5.3.
