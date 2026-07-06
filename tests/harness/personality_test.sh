#!/usr/bin/env bash
# Copyright (c) 2026 Brian J. Fox
# Licensed under GPLv2 with the GPLv2-AI Exception.

# personality_test.sh GNASH
#
# Behavioral test for the runtime `personality' / `emulate' builtin: switching
# the shell's personality mid-run, the -c and -L flags, and the emulate alias.

set -u
gnash=${1:?usage: personality_test.sh GNASH}
fails=0

# check DESCRIPTION EXPECTED  -- with the script on stdin, run under gnash
check() {
  local desc=$1 want=$2 got
  got=$("$gnash" --personality="$3" -c "$4" 2>&1)
  if [ "$got" != "$want" ]; then
    echo "FAIL: $desc" >&2
    echo "  want: [$want]" >&2
    echo "  got:  [$got]" >&2
    fails=$((fails + 1))
  fi
}

# no argument prints the current personality
check "report current (bash)" "bash" bash 'personality'
check "report current (zsh)"  "zsh"  zsh  'personality'

# switching changes expansion behavior live (word splitting differs)
check "switch bash->zsh affects splitting" $'[a][b][c]\n[a b c]' bash \
  'v="a b c"; printf "[%s]" $v; echo; personality zsh; printf "[%s]" $v; echo'

# -c runs a command under the given personality, then restores
check "-c runs under mode then restores" $'[x y]\nbash' bash \
  'personality zsh -c '\''v="x y"; printf "[%s]" $v; echo'\''; personality'

# -L makes the switch local to the enclosing function
check "-L restores on function return" $'zsh\nbash' bash \
  'f() { personality -L zsh; personality; }; f; personality'

# without -L the switch persists after the function returns
check "no -L leaks out of function" "zsh" bash \
  'g() { personality zsh; }; g; personality'

# emulate is the zsh-mode alias for personality
check "emulate alias works in zsh" "sh" zsh 'emulate sh; personality'

# emulate is not a builtin outside zsh mode (matches bash)
check "emulate absent in bash mode" "127" bash \
  'emulate zsh >/dev/null 2>&1; echo $?'

# switching to csh runs subsequent input through the csh interpreter
check "-c csh runs csh syntax" "b" bash \
  'personality csh -c '\''set x = (a b c); echo $x[2]'\'''

# an unknown personality name is rejected
check "unknown name is an error" "1" bash \
  'personality nonesuch >/dev/null 2>&1; echo $?'

if [ $fails -eq 0 ]; then
  echo "personality_test: all checks passed"
  exit 0
fi
echo "personality_test: $fails failure(s)" >&2
exit 1
