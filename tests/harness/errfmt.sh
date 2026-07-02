#!/usr/bin/env bash
# errfmt.sh GNASH BASH
#
# Differential check of *error-message format* (stderr) vs bash.  gnash prints
# its own program name ("gnash") where bash prints "bash"; that leading token is
# normalized so the rest of the format -- "line N:", the offending context, and
# the message text -- is compared exactly.
set -u

gnash=${1:?usage: errfmt.sh GNASH BASH}
bash=${2:?usage: errfmt.sh GNASH BASH}

# Each case is run with -c; only stderr is compared.
cases=(
  'nonexistent_cmd_xyz'
  'true; true; missing_cmd_2'
  'cd /no/such/dir'
  'readonly r=1; r=2'
  'set -u; echo $UNSET_XYZ'
  'echo hi > /no/such/dir/file'
  './this/does/not/exist'
  'type no_such_command_xyz'
)

# Replace the leading "<program name>:" token (which legitimately differs --
# gnash vs bash vs a full path) with a fixed marker on each line.
norm() { sed 's/^[^:]*:/SH:/'; }

fails=0
for c in "${cases[@]}"; do
  b=$("$bash"  -c "$c" 2>&1 >/dev/null | norm)
  g=$("$gnash" -c "$c" 2>&1 >/dev/null | norm)
  if [ "$b" != "$g" ]; then
    echo "ERRFMT MISMATCH: $c" >&2
    echo "  bash : $b" >&2
    echo "  gnash: $g" >&2
    fails=$((fails + 1))
  fi
done

if [ $fails -eq 0 ]; then
  echo "errfmt: all ${#cases[@]} error messages match bash (name-normalized)"
  exit 0
fi
echo "errfmt: $fails mismatch(es)" >&2
exit 1
