# gnash 1.6.0

The headline of this release is support for **named file-descriptor
redirections**.

## Named file-descriptor redirections

- The `{varname}` redirection form is now supported: `{varname}<`, `{varname}>`,
  `{varname}>>`, `{varname}<>`, `{varname}>&N`, `{varname}<&N`, `{varname}>&-`,
  `{varname}<&-`, and `{varname}<<EOF`.
- gnash allocates a fresh descriptor (a high, close-on-exec number), opens the
  redirection on it, and stores the number in `varname`:
  ```sh
  exec {fd}>logfile      # fd now holds e.g. 10
  echo "line" >&$fd
  exec {fd}>&-           # close the descriptor named by fd
  ```
- With `exec` the descriptor persists; for an ordinary command it is closed
  afterwards while the variable keeps the number. `{varname}>&-` / `{varname}<&-`
  close the descriptor the variable currently names. Works on simple and
  compound commands, and `declare -f` reproduces the `{name}` form.

  Previously `{fd}>file` was taken as an ordinary word (`exec: {fd}: not found`).
