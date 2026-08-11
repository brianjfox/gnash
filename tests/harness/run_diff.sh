#!/usr/bin/env bash
# Copyright (c) 2026 Brian J. Fox
# Licensed under GPLv2 with the GPLv2-AI Exception.

# run_diff.sh GNASH BASH
#
# Differential execution: run each script under gnash and bash, comparing
# stdout and exit status.  These scripts are all within gnash's implemented
# feature set, so they must match exactly.  Exit 0 if all agree.
set -u

gnash=${1:?usage: run_diff.sh GNASH BASH}
bash=${2:?usage: run_diff.sh GNASH BASH}

# The oracle must be a REAL bash.  On a dev box a gnash symlink often shadows
# `bash' on $PATH (so CMake's find_program hands us gnash, not bash); a
# differential against gnash itself is meaningless.  If the given oracle reports
# itself as gnash, look for a real bash: $GNASH_BASH, then a Homebrew Cellar
# bash, then the usual locations.  If none is found, skip rather than fail.
is_real_bash() { "$1" --version 2>/dev/null | head -1 | grep -qi 'GNU bash'; }
if ! is_real_bash "$bash"; then
  set +u  # GNASH_BASH may be unset
  found=""
  for cand in "$GNASH_BASH" /opt/homebrew/Cellar/bash/*/bin/bash \
              /usr/local/bin/bash /opt/local/bin/bash /bin/bash; do
    if [ -n "$cand" ] && [ -x "$cand" ] && is_real_bash "$cand"; then found=$cand; break; fi
  done
  set -u
  if [ -z "$found" ]; then
    echo "run_diff: no real bash oracle found (got '$bash'); skipping" >&2
    exit 0
  fi
  bash=$found
fi

scripts=(
  'echo hello world'
  'echo -n no-newline'
  'echo a b c | wc -w'
  'printf "%s-%d\n" foo 42'
  'x=5; y=3; echo $((x*y + x - y))'
  'echo $((2**10)) $((17%5)) $((1<<4))'
  'echo $((3 > 2)) $((3 < 2)) $((5 == 5))'
  'x=10; echo $((x++)) $((x++)) $x'
  'i=0; s=0; while [ $i -lt 5 ]; do s=$((s+i)); i=$((i+1)); done; echo $s'
  'for i in 1 2 3 4 5; do printf "%s " $((i*i)); done; echo'
  'for ((i=0;i<4;i++)); do printf "%d " $i; done; echo'
  'echo {a,b,c}{1,2}'
  'echo {1..10}'
  'echo {5..1}'
  'echo {9223372036854775805..9223372036854775807}'
  'echo {-9223372036854775806..-9223372036854775808}'
  'echo pre{X,Y,Z}post'
  'v=HelloWorld; echo ${#v}'
  'v=HelloWorld; echo ${v:2:5}'
  'v=HelloWorld; echo ${v^^} ${v,,}'
  'f=archive.tar.gz; echo ${f%.gz} ${f%%.*} ${f#*.} ${f##*.}'
  'p=a/b/c/d; echo ${p##*/} ${p%/*}'
  's=aXbXcXd; echo ${s/X/-} ${s//X/-}'
  'unset u; echo ${u:-default} ${u:=assigned}; echo $u'
  'x=set; echo ${x:+yes} ${x:-no}'
  'echo $(echo one; echo two)'
  'echo "$(echo nested $(echo deep))"'
  'a="x  y   z"; for w in $a; do echo "[$w]"; done'
  'a="x  y   z"; for w in "$a"; do echo "[$w]"; done'
  'set -- p1 p2 p3; echo $# "$@"; echo $1-$3'
  'set -- a b c; for x in "$@"; do echo "arg=$x"; done'
  'if true && false; then echo t; else echo f; fi'
  'true || echo fallback'
  'false && echo nope; echo after'
  'case hello in h*) echo H;; *) echo other;; esac'
  'case xyz in a) echo a;; x*|y*) echo xy;; esac'
  'greet() { echo "hi, $1"; return 0; }; greet Alice; echo $?'
  'add() { echo $(($1 + $2)); }; add 3 4'
  'count() { local n=$1; echo $n; }; count 7 2>/dev/null || echo 7'
  'echo test > /tmp/gnash_rd_$$; cat /tmp/gnash_rd_$$; rm -f /tmp/gnash_rd_$$'
  'printf "%s\n" line1 line2 | while read L; do echo "got:$L"; done'
  'x=5; [[ $x -gt 3 && $x -lt 10 ]] && echo inrange'
  '[[ foobar == foo* ]] && echo prefix; [[ abc != xyz ]] && echo neq'
  'echo $(( 10 / 3 )).$(( 10 % 3 ))'
  'n=3; r=1; for ((k=1;k<=n;k++)); do r=$((r*k)); done; echo $r'
  'echo abc | tr a-z A-Z | rev'
  's="  trim  "; echo "[${s#  }]"'
  'a=1; b=2; c=$((a+b)); d=$((c*c)); echo $a $b $c $d'
  'a=(one two three); echo ${a[1]} ${#a[@]} "${a[@]}"'
  'a=(1 2 3); a+=(4 5); echo "${a[@]}" count=${#a[@]}'
  'a=(x y z); for i in ${!a[@]}; do echo "$i=${a[$i]}"; done'
  'declare -A m; m[apple]=1; m[banana]=2; echo ${m[apple]}+${m[banana]}=${#m[@]}'
  'i=0; for w in a b c d; do arr[i++]=$w; done; echo "${arr[@]}"'
  'f() { local x=in; echo $x; }; x=out; f; echo $x'
  'g() { local -i n=5; echo $((n*n)); }; g'
  'set -e; false; echo unreached'
  'set -e; true; echo reached'
  'set -e; if false; then echo x; fi; echo ok'
  'set -e; false || echo caught; echo done'
  'let "a = 3 + 4 * 2"; echo $a'
  'readonly PI=3; echo $PI'
  's=Hello; declare -l lower=nope; echo ${s,,}'
  'n=5; fact=1; for ((k=2;k<=n;k++)); do ((fact*=k)); done; echo $fact'
  'echo a b c | { read x y z; echo "$z $y $x"; }'
  '(echo bg) & wait; echo fg'
  'true & wait $!; echo "status=$?"'
  'x=$(echo a; echo b); echo "$x"'
  'echo start; seq 1 3 | while read n; do echo "n=$n"; done; echo end'
  'sleep 5 & p=$!; kill "$p"; wait "$p" 2>/dev/null; echo reaped'
  'n=$(seq 1 10 | wc -l); echo "lines=$n"'
  'echo one two three | tr " " "\n" | sort | head -1'
  'RANDOM=42; echo $RANDOM $RANDOM $RANDOM $RANDOM'
  'RANDOM=1; echo $((RANDOM%100)) $((RANDOM%1000)) $RANDOM'
  'RANDOM=12345; for i in 1 2 3; do printf "%s " $RANDOM; done; echo'
  'echo $LINENO; echo $LINENO; x=1; echo $LINENO'
  's=$SECONDS; echo "$s"; echo $((SECONDS>=0))'
  'echo $(($$ == $$)) $((BASHPID>0)) $((EPOCHSECONDS>1000000000))'
  'printf "%q " hello "a b" "it'\''s" "" "*.x" "\$v" "a,b" "~x"; echo'
  'printf "%b|" "\101" "\0101" "\x41" "a\tb"; echo'
  'printf -v out "%s=%05d" foo 7; echo "[$out]"'
  'printf "%-8s|%5.2f|%3d\n" hi 3.14159 42'
  # printf %(FMT)T time conversion (epoch arg; both shells share the ambient TZ)
  'printf "%(%Y-%m-%d)T\n" 0'
  'printf "[%(%H:%M:%S)T]\n" 0'
  'printf "pre-%d-%(%Y)T-post\n" 5 0'
  'echo "*.md"; echo '\''*.md'\''; x="*"; echo "$x"'
  'printf "<%s>" "" a ""; echo'
  'set -- "" x ""; echo $#; for a in "$@"; do echo "[$a]"; done'
  'v="a b"; printf "<%s>" "$v"; echo; printf "<%s>" $v; echo'
  'v="Hello World"; echo "${v@Q}" "${v@U}" "${v@u}" "${v@L}"'
  'v="it'\''s a test"; echo "${v@Q}"; echo "[${nope@Q}]"'
  'declare -i n=5; declare -r r=x; a=(1 2); declare -A m=([k]=v); echo "${n@a}${r@a}${a@a}${m@a}"'
  'export ev="x y"; echo "${ev@A}"; p=hi; echo "${p@A}"'
  '[[ "2024-01-15" =~ ([0-9]+)-([0-9]+)-([0-9]+) ]] && echo "${BASH_REMATCH[1]}/${BASH_REMATCH[2]}/${BASH_REMATCH[3]}"'
  're="^[0-9]+$"; [[ 12345 =~ $re ]] && echo num; [[ 12x =~ $re ]] || echo notnum'
  'v=Test123; [[ $v =~ ([A-Za-z]+)([0-9]+) ]]; echo "${BASH_REMATCH[1]}/${BASH_REMATCH[2]} n=${#BASH_REMATCH[@]}"'
  '[[ cat =~ ^(cat|dog)$ ]] && echo "${BASH_REMATCH[1]}"; [[ "a.b" =~ a\.b ]] && echo dot'
  'cd /tmp; pwd; cd /usr/bin; cd ..; pwd'
  'cd /usr/bin; echo "$PWD"; cd ../lib; echo "$PWD"'
  'cd /tmp && pushd /usr >/dev/null && pushd /bin >/dev/null; echo "n=$(dirs | wc -w)"; popd >/dev/null; popd >/dev/null; pwd'
  'cd /tmp; pushd /usr >/dev/null; pushd /bin >/dev/null; pushd +1 >/dev/null; pwd'
  'printf "a\nb\nc\n" > /tmp/gm.$$; mapfile -t x < /tmp/gm.$$; printf "<%s>" "${x[@]}"; echo " n=${#x[@]}"; rm -f /tmp/gm.$$'
  'printf "1\n2\n3\n4\n" > /tmp/gm.$$; mapfile -t -s1 -n2 y < /tmp/gm.$$; echo "${y[*]}"; rm -f /tmp/gm.$$'
  'printf "p:q:r:" > /tmp/gm.$$; readarray -t -d: z < /tmp/gm.$$; printf "[%s]" "${z[@]}"; echo; rm -f /tmp/gm.$$'
  'cat <(echo hello)'
  'diff <(printf "a\nb\nc\n") <(printf "a\nX\nc\n"); echo "rc=$?"'
  'wc -l < <(seq 1 5) | tr -d " "'
  'while read l; do echo "got:$l"; done < <(printf "p\nq\n")'
  'sort <(echo 3; echo 1; echo 2) | tr "\n" ","; echo'
  'declare -a arr=(1 2 3); echo "${#arr[@]} ${arr[1]}"'
  'declare -a a=(one "two three" four); echo "${#a[@]}: ${a[1]}"'
  'declare -A m=([x]=1 [y]=2); echo "${m[x]}${m[y]} ${#m[@]}"'
  # an unquoted space inside a compound-initializer subscript is part of the key
  'declare -A m=([a b]="c d"); echo "[${m[a b]}]"'
  'declare -A m=([one]=1 [two words]=2 [x]=3); echo "${m[two words]} ${#m[@]}"'
  'declare -A m=([a b]=1 [c d]=2); echo "${m[a b]}${m[c d]} n=${#m[@]}"'
  'y="hi there"; declare z=$y; echo "$z"; declare -i n=3+4; echo "$n"'
  'f(){ local -a la=(p q r); echo "${la[2]}"; }; f'
  'x=${ echo hello world; }; echo "[$x]"'
  'n=0; y=${ n=42; echo captured; }; echo "$y n=$n"'
  'v=${ for i in 1 2 3; do echo -n "$i"; done; }; echo "$v"'
  'a=(foo bar baz); echo "${a[@]^}"; echo "${a[@]^^}"'
  'a=(file.txt doc.pdf); echo "${a[@]%.*}"; echo "${a[@]#*.}"'
  'a=(one two); echo "${a[@]/o/O}"; echo "${a[*]^^}"'
  'a=(hi bye); echo "${a[@]@Q}"'
  'trap "echo caught" USR1; kill -USR1 $$; echo done'
  'x=5; trap "echo x=$x" USR1; kill -USR1 $$'
  'trap "echo A" USR1; kill -USR1 $$; trap - USR1; kill -USR1 $$ 2>/dev/null; echo end'
  'trap "echo bye" EXIT; echo body'
  'echo $BASH_SUBSHELL; ( echo $BASH_SUBSHELL ); echo $BASH_SUBSHELL | cat; { echo $BASH_SUBSHELL; } | cat'
  'echo "$(echo $BASH_SUBSHELL) $( ( echo $BASH_SUBSHELL ) )"'
  'echo "n=${#BASH_VERSINFO[@]} status=${BASH_VERSINFO[4]}"; [ ${BASH_VERSINFO[0]} -ge 4 ] && echo modern'
  'set -o | wc -l | tr -d " "'
  'qzv=hi; wzv="x y"; set | grep -E "^(qzv|wzv)="'
  'set -- x y z; echo "$# $*"; shift; echo "$@"; set a b; echo "$1$2"'
  'type -t if cd ls; type -t no_such_cmd_zz 2>/dev/null; echo "rc=$?"'
  'type -p ls; echo "cdp=[$(type -p cd)]"; type -P ls'
  'type -a echo; echo "---"; type -at echo'
  'cd(){ :; }; type -t cd; type -f cd; echo "ff=[$(type -f cd 2>/dev/null | head -1)]"'
  'type if; type cd; type ls'
  'help -s cd; help -d pushd popd; help -s shopt'
  'cd(){ echo fn; }; builtin cd /tmp && pwd; builtin echo direct'
  # `command NAME args' runs NAME with its argv verbatim -- no re-parsing of the
  # already-expanded, quoted words (regression: it used to re-join+re-lex them,
  # mangling backslash escapes, quoted whitespace, and embedded metacharacters).
  'nvm_echo() { command printf %s\\n "$*"; }; nvm_echo "unknown version dir"'
  'command printf "%s\n" "a  b|c;d(e)"'
  'printf(){ echo shadow; }; command printf "%s\n" real'
  'command command echo nested; command; echo "bare=$?"'
  'command -v printf; command -v ls >/dev/null && echo ls-found; command -v nosuch_xyz; echo "rc=$?"'
  'command -V cd; command -V nosuch_xyz 2>/dev/null; echo "rc=$?"'
  'f(){ echo fn; }; command -v f; command f 2>/dev/null; echo "cmd-f-rc=$?"'
  # $- reports the option flags in bash order (default hashall/braceexpand on,
  # then the invocation letter -- `c' under -c).
  'echo "[$-]"'
  'set -e; echo "[$-]"'
  'set -efuvx; echo "[$-]" 2>/dev/null; set +efuvx; echo "[$-]"'
  'case $- in *c*) echo has-c;; *) echo no-c;; esac; case $- in *i*) echo interactive;; *) echo noninteractive;; esac'
  # A non-login shell reports login_shell off (matches bash under -c).
  'shopt -q login_shell && echo login || echo nonlogin'
  # Alias expansion only applies in command position: a `for'/`select'/`case'
  # variable that happens to name an alias must stay literal (regression: the
  # loop variable was being alias-expanded, breaking `for j in ...; do').  The
  # alias must be active before the loop is *parsed*, so it is defined here and
  # the loop is sourced from a separate file, as when an rc file sources nvm.sh.
  'shopt -s expand_aliases; alias j="jobs -l"
cat > /tmp/gnrx1.$$ <<"EOF"
for j in a b c; do printf "%s " "$j"; done; echo
EOF
source /tmp/gnrx1.$$; rm -f /tmp/gnrx1.$$'
  'shopt -s expand_aliases; alias j="jobs -l"
cat > /tmp/gnrx2.$$ <<"EOF"
case j in j) echo matched;; *) echo no;; esac
EOF
source /tmp/gnrx2.$$; rm -f /tmp/gnrx2.$$'
  # An alias in real command position must still expand (guard against over-
  # suppressing the fix above).
  'shopt -s expand_aliases; alias greet="echo expanded-ok"
cat > /tmp/gnrx3.$$ <<"EOF"
greet
EOF
source /tmp/gnrx3.$$; rm -f /tmp/gnrx3.$$'
  'shopt | wc -l | tr -d " "; shopt -q nullglob; echo $?; shopt -s nullglob; shopt -q nullglob; echo $?'
  'shopt -p nullglob extglob dotglob'
  'cd /tmp; set -- nomatch_glob_*.zz; echo "off=$#"; shopt -s nullglob; set -- nomatch_glob_*.zz; echo "on=$#"'
  'hash ls; hash -t ls'
  'ulimit -a'
  'ulimit -n; ulimit -Sf; ulimit -c'
  'enable -n echo; echo hi; enable echo; echo back'
  'f(){ set -- $(caller 0); echo "$1 $2"; }; g(){ f; }; g'
  'alias ll="ls -l"; alias x=y; alias'
  'alias a=1 b="two words"; alias a; alias b'
  'alias q=1; unalias q; unalias -a; alias; echo "done=$?"'
  'HISTFILE=/dev/null; history -c; history -s "echo one"; history -s "echo two"; history'
  'HISTFILE=/dev/null; history -c; history -s cmd1; history -s cmd2; fc -l'
  'HISTFILE=/dev/null; history -c; history -s "ls -la"; history -p "!!"'
  'HISTFILE=/dev/null; history -c; history -s a; history -s b; history -s c; history -d 2; history'
  'compgen -W "apple apricot banana" a'
  'compgen -W "one two three"; echo "rc=$?"'
  'compgen -P pre- -S -suf -W "a b" a; compgen -W "x y z" q; echo "nomatch=$?"'
  'complete -W "alpha beta" mycmd; complete -p mycmd'
  'bind -l 2>/dev/null | grep -c beginning-of-line'
  # BASH_* introspection variables
  'shopt -s extdebug; f(){ echo "[${BASH_ARGC[@]}] [${BASH_ARGV[@]}]"; }; f a b c'
  'shopt -s extdebug; g(){ echo "[${BASH_ARGC[@]}] [${BASH_ARGV[@]}]"; }; f(){ g x y; }; f a b c'
  'shopt -s extdebug; f(){ echo "${BASH_ARGC[0]} ${#BASH_ARGV[@]} ${BASH_ARGV[0]}"; }; f a b c'
  'f(){ echo "[${BASH_ARGC[@]}][${BASH_ARGV[@]}]"; }; f a b c'
  'alias ll="ls -l" gg=grep; echo "${!BASH_ALIASES[@]}|${BASH_ALIASES[ll]}|${#BASH_ALIASES[@]}"'
  'hash -p /bin/ls ls; echo "${BASH_CMDS[ls]}|${#BASH_CMDS[@]}"'
  'echo "$BASHOPTS" | tr : "\n" | sort'
  'shopt -s extglob nullglob; echo "$BASHOPTS" | tr : "\n" | grep -E "extglob|nullglob"'
  'echo "${BASH_VERSINFO[3]} ${BASH_VERSINFO[4]}"'
  # pipefail: pipeline status is the last non-zero stage (0 if all succeed)
  'set -o pipefail; false | true; echo $?'
  'set -o pipefail; true | false; echo $?'
  'set -o pipefail; true | true; echo $?'
  'set -o pipefail; (exit 3) | true | (exit 5); echo $?'
  'false | true; echo $?'
  'set -o pipefail; set +o pipefail; false | true; echo $?'
  # exec resolves the command through the shell $PATH, and its flags
  'd=/tmp/gnash_exec_rd; rm -rf "$d"; mkdir -p "$d"; printf "#!/bin/sh\necho exec-ok\n" > "$d/xp"; chmod +x "$d/xp"; PATH="$d:$PATH"; exec xp'
  'exec -a ZEROTH /bin/sh -c "echo argv0=\$0"'
  # quoted "$@" / "${a[@]}" keep empty elements
  'set -- "" x ""; printf "<%s>" "$@"; echo'
  'a=(p "" q); for e in "${a[@]}"; do echo "[$e]"; done'
  # defaulting/alternative operators on a whole array (not a slice)
  'a=(); echo "${a[@]:-DEF}"; echo "${a[@]-DEF}"'
  'a=(""); echo "[${a[@]:-DEF}]"; echo "[${a[@]-DEF}]"'
  'a=(x y); echo "${a[@]:-DEF}" "${a[@]:+ALT}"'
  'a=(); echo "${a[@]:+ALT}"; echo "${a[*]:-D E F}"'
  'a=(); printf "<%s>" ${a[@]:-x y}; echo; a=(p q); printf "<%s>" "${a[@]:-z}"; echo'
  # ulimit reports a single resource as a bare value
  'ulimit -n; ulimit -c'
  # set -u: defaulting operators handle an unset variable (no nounset error)
  'set -u; echo "${UNSET:-def}" "${UNSET-d2}" "${UNSET:+a}" "${OTHER:=x}"; echo "$OTHER"'
  'set -u; v=hi; echo "${v:-nope}${v:+yes}"'
  # set -u: a genuine unbound reference (and ${x?}) is fatal with status 127
  'set -u; echo pre; echo "$UNSET"; echo post'
  'echo pre; echo "${UNSET?boom}"; echo post'
  # read: backslash escapes quote IFS chars; trailing backslash before EOF drops
  'echo " a  b\ " | { read x y; echo "-$x-$y-"; }'
  'echo " a  b\ " | { read x; echo "-$x-"; }'
  'printf "abc\\\\" | { read v; echo "[$v]"; }'
  'printf "ab\\\\cd" | { read v; echo "[$v]"; }'
  # read: exit status is 1 at EOF without delimiter, 0 with it
  'printf "abc" | { read v; echo "$? [$v]"; }'
  'printf "abc\n" | { read v; echo "$? [$v]"; }'
  # read: the last variable takes a lone final field dequoted, more fields raw
  'IFS=: read x y z <<< ":::"; echo "[$x][$y][$z]"'
  'IFS=: read v rest <<< "a:b:c d"; echo "[$v][$rest]"'
  'IFS=: read v <<< ":abc:"; echo "[$v]"'
  # read: -N assigns without splitting; -n stops at the count
  'printf "a b c d\n" | { read -N 5 v; echo "[$v]"; }'
  'printf "abcdefg\n" | { read -n 3 v; echo "[$v]"; read rest; echo "[$rest]"; }'
  # read: -t 0 polls for available input; bad -t / -n are usage errors
  'echo data | { read -t 0; echo $?; }'
  'read -t -3 v 2>/dev/null <<< x; echo $?'
  'read -n -1 v 2>/dev/null <<< x; echo $?'
  # read: readonly target stops assignment with status 2
  'readonly RB; read RA RB RC 2>/dev/null <<< "1 2 3"; echo "$? [$RA][$RC]"'
  # read into REPLY keeps the line unsplit and unstripped
  'echo " A B " | { read; echo "[$REPLY]"; }'
  # exec redirections are permanent in the current shell
  'echo file-line > /tmp/gnash_exec_$$; exec < /tmp/gnash_exec_$$; read v; echo "[$v]"; rm -f /tmp/gnash_exec_$$'
  # bare `>&file'"'"' redirects BOTH stdout and stderr to the file (= `&>file'"'"'),
  # not an fd dup: both streams land in the file.  Regression for the batch95
  # ambiguous-redirect check that wrongly rejected the filename form (#313).
  '{ echo out; echo err >&2; } >&/tmp/gnash_rboth_$$; cat /tmp/gnash_rboth_$$; rm -f /tmp/gnash_rboth_$$'
  'echo discarded >&/dev/null; echo kept'
  # an explicit source fd (`2>&file'"'"') or an input dup (`<&file'"'"') with a
  # non-fd word stays an ambiguous redirect (rc 1, the command does not run).
  'echo hi 2>&/dev/null; echo "rc=$?"'
  'cat <&/dev/null; echo "rc=$?"'
  # printf: octal/hex escapes in the format string; %q of nonprintable bytes
  'LC_ALL=C printf "%s\n" "$(printf "a\101b")"'
  'printf "x\11y\n" | cat -v'
  'LC_ALL=C printf "<%q>\n" "$(printf "B\315")"'
  'LC_ALL=C printf "<%q>\n" "$(printf "tab\thigh\200")"'
  # printf %b arguments still handle \0NNN
  'printf "%b\n" "A\0101Z" | cat -v'
  # ${!name} indirection, with operators, and ${!prefix*} name listing
  'x=y; y=hello; echo "${!x}"'
  'x=unset_zz; echo "${!x:-fallback}"'
  'ab1=v1; ab2=v2; echo ${!ab*}'
  'set -- one two; n=2; echo "${!n}"'
  # indirection through the count $# and through a positional-parameter digit
  'set -- w x y z; echo "${!#}"'
  'set -- foo bar; foo=HELLO; echo "${!1}"'
  # a bare `!' negates the null command; `! cmd' inverts
  '! ; echo $?'
  '! true; echo $?'
  '! false; echo $?'
  # a here-document delimited by end-of-file still runs (with a warning)
  'cat <<XEOF 2>/dev/null
line1'
  # A sourced file fires the RETURN trap even without functrace, but does not
  # inherit the DEBUG trap unless functrace is on.  The last case: a trap the
  # function installs for ITSELF fires for a `source' in its body, twice --
  # once leaving the sourced file, once leaving the function.
  'd=$(mktemp -d); printf "echo in-sub\n" >"$d/s"; trap "echo RET" RETURN; source "$d/s"; echo after; rm -rf "$d"'
  'd=$(mktemp -d); printf "echo one\necho two\n" >"$d/s"; trap "echo DBG" DEBUG; source "$d/s"; trap - DEBUG; rm -rf "$d"'
  'd=$(mktemp -d); printf "echo one\n" >"$d/s"; set -T; trap "echo DBG" DEBUG; source "$d/s"; trap - DEBUG; set +T; rm -rf "$d"'
  'd=$(mktemp -d); printf "echo in-sub\n" >"$d/s"; f() { trap "echo RET" RETURN; source "$d/s"; }; f; echo after; rm -rf "$d"'
  # A child process does not inherit the DEBUG/RETURN traps without functrace.
  # For a command substitution that is visible in the VALUE: the trap would
  # write into the capture pipe, so `x=$(echo hi)' would come back as "DBG hi".
  'trap "echo DBG" DEBUG; x=$(echo hi; echo there); echo "x=[$x]"'
  'trap "echo DBG" DEBUG; x=`echo hi`; echo "x=[$x]"'
  'f(){ echo F; }; trap "echo DBG" DEBUG; x=$(f); echo "x=[$x]"'
  'set -T; f(){ echo F; }; trap "echo DBG" DEBUG; x=$(f); echo "x=[$x]"'
  'trap "echo RET" RETURN; f(){ echo F; }; x=$(f); echo "x=[$x]"'
  'trap "echo DBG" DEBUG; (echo sub); echo after'
  'set -T; trap "echo DBG" DEBUG; (echo sub); echo after'
  # ...but a pipeline stage and a background job still report their own DEBUG
  # trap, which gnash fires in the child rather than the parent.
  'trap "echo DBG" DEBUG; echo a | cat'
  'trap "echo DBG" DEBUG; echo a & wait'
  # Arithmetic: a character that cannot begin an operand and is not an operator
  # is the tokenizer'"'"'s "invalid arithmetic operator"; one that can begin an
  # operand is read as a token and rejected by the grammar instead.
  'echo $(( x@y )) 2>&1'
  'echo $(( 1 @ 2 )) 2>&1'
  'echo $(( 1 ] )) 2>&1'
  'echo $(( x y )) 2>&1'
  'echo $(( 1 2 3 )) 2>&1'
  'echo $(( x!!y )) 2>&1'
  'echo $(( 1+ )) 2>&1'
  'echo $(( x= )) 2>&1'
  # The line a diagnostic reports.  A command that installs no line of its own
  # -- `while'"'"', `if'"'"', a function definition -- is reported against the line the
  # PARSER stopped on, so a redirection error on a multi-line `while ... done > f'"'"'
  # names the `done'"'"'.  Inside a subshell that line is the subshell'"'"'s closing
  # paren, which is why the `for'"'"' and the posix funcdef below both report it
  # rather than their own line.  (These pipe stderr through sed because the
  # harness compares stdout only, and the two shells name themselves.)
  '{ d=$(mktemp -d); cd "$d"; touch nw; chmod a-w nw; while [ -z x ]; do
 y=4
done > nw
cd /; rm -rf "$d"; } 2>&1 | sed "s/^[^ ]*: //"'
  '{ d=$(mktemp -d); cd "$d"; touch nw; chmod a-w nw; (set -e
for f in 1 2; do y=4; done > nw
echo after: $?)
cd /; rm -rf "$d"; } 2>&1 | sed "s/^[^ ]*: //"'
  '{ set -o posix; ( break()
{
 :
}
); } 2>&1 | sed "s/^[^ ]*: //"'
  '{ if true; then
 y=1
fi > /nonexistent-dir-xyz/f; } 2>&1 | sed "s/^[^ ]*: //"'
  # $LINENO itself is unaffected: each simple command still reports its own line.
  '( echo "L=$LINENO"
echo "L=$LINENO"
)
echo "L=$LINENO"'
  # An unterminated quote, backquote or `${' names the line it OPENED on; an
  # unterminated `$(' names where input ran out.  These run a script file (via
  # "$0", the shell under test) because a syntax error aborts -c before any
  # pipeline could filter the message.
  'd=$(mktemp -d); printf "foo=bar\necho \"\${foo:-\"a}\"\n" > "$d/s"; "$0" "$d/s" 2>&1 | sed "s|^[^ ]*: ||"; rm -rf "$d"'
  'd=$(mktemp -d); printf "foo=bar\necho \`bar\n" > "$d/s"; "$0" "$d/s" 2>&1 | sed "s|^[^ ]*: ||"; rm -rf "$d"'
  'd=$(mktemp -d); printf "foo=bar\necho \"abc\n" > "$d/s"; "$0" "$d/s" 2>&1 | sed "s|^[^ ]*: ||"; rm -rf "$d"'
  'd=$(mktemp -d); printf "foo=bar\necho \$(bar\nbaz\n" > "$d/s"; "$0" "$d/s" 2>&1 | sed "s|^[^ ]*: ||"; rm -rf "$d"'
  # $BASH_XTRACEFD sends `set -x'"'"' output to a descriptor other than stderr; a
  # value that is not an open fd is an error and leaves the destination alone.
  # `unset'"'"' puts it back on stderr -- note the trace of the `unset'"'"' itself is
  # the last line still written to the file.
  'd=$(mktemp -d); exec 4>"$d/t"; BASH_XTRACEFD=4; set -x; echo one; set +x; exec 4<&-; echo "file:"; cat "$d/t"; rm -rf "$d"'
  'd=$(mktemp -d); exec 4>"$d/t"; BASH_XTRACEFD=4; set -x; echo one; unset BASH_XTRACEFD; echo two; set +x; exec 4<&-; echo "file:"; cat "$d/t"; rm -rf "$d"'
  '{ BASH_XTRACEFD=9; } 2>&1 | sed "s|^[^ ]*: ||"'
  '{ BASH_XTRACEFD=abc; } 2>&1 | sed "s|^[^ ]*: ||"'
  # An EMPTY array subscript is diagnosed but is NOT fatal: a read yields 0, a
  # write is skipped while the assignment still evaluates to its right-hand
  # side, and the command list keeps running.
  '{ declare -a a; echo $(( a[""]=2+3 )); } 2>&1 | sed "s|^[^ ]*: ||"'
  '{ declare -a a; echo $(( 7 + a[""] )); echo AFTER; } 2>&1 | sed "s|^[^ ]*: ||"'
  'declare -a a; { (( a[""]=24 )); } 2>&1 | sed "s|^[^ ]*: ||"; declare -p a'
  # ...but a subscript that fails to EVALUATE does unwind the list, even from
  # `let'"'"', where a bad top-level expression merely returns non-zero.  Under
  # assoc_expand_once the quotes survive into the evaluator, so this is a
  # syntax error rather than a write to element 0.
  'shopt -s assoc_expand_once; declare -a a; a[0]=0; { ( let "a[\" \"]"=18 ; echo AFTER ); } 2>&1 | sed "s|^[^ ]*: ||"; declare -p a'
  'shopt -u assoc_expand_once; declare -a a; a[0]=0; { ( let "a[\" \"]"=18 ; echo AFTER ); } 2>&1 | sed "s|^[^ ]*: ||"; declare -p a'
  '{ let "x+"; } 2>&1 | sed "s|^[^ ]*: ||"; echo AFTER'
  # `trap SIGSPEC' with a single operand REVERTS that signal; a single operand
  # that names no signal is an ACTION with nothing to apply it to, which is a
  # usage error (status 2), not a silent no-op.
  'trap "echo hi" USR1; trap USR1; trap -p | grep -c USR1'
  '{ trap ""; } 2>&1; echo "rc=$?"'
  '{ trap 512; } 2>&1; echo "rc=$?"'
  '{ trap foo; } 2>&1; echo "rc=$?"'
  'trap 15; echo "rc=$?"'
  # posix interp 1602: a BARE `return' in a signal trap action reports $? from
  # before the trap ran -- the action cannot change it.  An explicit argument
  # still wins, and a function called BY the action reports its own status.
  'setexit() { return "$1"; }; trap "setexit 111; return" USR1; invoke() { kill -USR1 $$; return 222; }; invoke; echo "dollar=$?"'
  'setexit() { return "$1"; }; trap "setexit 111; return 7" USR1; invoke() { kill -USR1 $$; return 222; }; invoke; echo "dollar=$?"'
  'setexit() { return "$1"; }; handler() { setexit 111; return; }; trap "handler; stat=\$?; return" USR1; invoke() { kill -USR1 $$; return 222; }; invoke; echo "stat=$stat"'
  # xtrace repeats PS4'"'"'s FIRST character once per nesting level, so a trace from
  # inside an `eval'"'"' or a trap action reads `++'"'"'.  A function call and a plain
  # subshell do NOT nest; a command substitution does.
  'PS4="+ "; set -x; eval "echo hi"'
  'PS4="- "; set -x; eval "echo hi"'
  'PS4="+ "; set -x; f(){ echo in; }; f'
  'PS4="+ "; set -x; (echo sub)'
  'PS4="+ "; set -x; x=$(echo cs); echo "$x"'
  'PS4=""; set -x; echo empty'
  # `declare -ft NAME'"'"' sets the TRACE attribute: that function inherits the DEBUG
  # and RETURN traps with no `set -T'"'"'.  It sets rather than displays, and a
  # missing function fails silently (unlike `readonly -f'"'"').
  'f(){ echo in; }; declare -ft f; trap "echo D" DEBUG; f; trap "" DEBUG'
  'f(){ echo in; }; trap "echo D" DEBUG; f; trap "" DEBUG'
  'f(){ echo in; }; declare -ft f; trap "echo R" RETURN; f; trap - RETURN'
  'f(){ :; }; declare -ft f; declare -F f'
  'f(){ :; }; declare -ft f; declare +t f; echo "rc=$?"'
  '{ declare -ft nosuch; } 2>&1; echo "rc=$?"'
  # A signal ignored when the shell started can be neither trapped nor reset.
  'trap "" USR2; "$0" -c '"'"'trap "echo USR2" USR2; trap -p USR2'"'"''
  'trap "echo U" USR2; trap -p USR2; trap - USR2; trap -p USR2; echo end'
  # `echo'"'"' reports a failed write.  Duplicating a READ-ONLY descriptor onto
  # stdout succeeds, so `>&3'"'"' below fails only when the write happens.
  '{ exec 3</etc/passwd; echo hi >&3; } 2>&1 | sed "s|^[^ ]*: ||"; echo "rc=$?"'
  'exec 3>/dev/null; echo hi >&3; echo "rc=$?"'
  'echo normal; echo "rc=$?"'
  # `cd -'"'"' echoes where it went, but only once the chdir SUCCEEDS.
  '{ OLDPWD=/tmp/cd-notthere; cd -; } 2>&1 | sed "s|^[^ ]*: ||"; echo "rc=$?"'
  'cd /tmp; cd /usr; cd -; echo "rc=$?"; pwd'
  '{ unset OLDPWD; cd -; } 2>&1 | sed "s|^[^ ]*: ||"; echo "rc=$?"'
  # Who a readonly failure names depends on WHO performed the assignment.  With
  # an explicit -a, an unquoted compound is the command'"'"'s own assignment word
  # (the enclosing function answers) while a quoted one is the builtin'"'"'s own
  # (it names itself).  Without -a there is no attribution at all.
  '{ a=(x); readonly a; f() { readonly -a a=(2); }; f; } 2>&1 | sed "s|^[^ ]*: ||"'
  '{ a=(x); readonly a; f() { readonly -a "a=(4)"; }; f; } 2>&1 | sed "s|^[^ ]*: ||"'
  '{ a=(x); readonly a; f() { readonly a=(4); }; f; } 2>&1 | sed "s|^[^ ]*: ||"'
  '{ a=(x); readonly a; f() { readonly "a=(5)"; }; f; } 2>&1 | sed "s|^[^ ]*: ||"'
  '{ a=(x); readonly a; readonly -a a=(2); } 2>&1 | sed "s|^[^ ]*: ||"'
  '{ a=x; readonly a; f() { readonly a=9; }; f; } 2>&1 | sed "s|^[^ ]*: ||"'
  # `unset -n'"'"' names a NAMEREF to remove; applied to a variable that is not one
  # it does NOTHING, rather than falling back to unsetting it.
  'y=2; unset -n y; declare -p y'
  'declare -n r=t; t=5; unset -n r; { declare -p r; } 2>&1 | sed "s|^[^ ]*: ||"; declare -p t'
  '{ y=2; unset y; declare -p y; } 2>&1 | sed "s|^[^ ]*: ||"'
  'unset -n nosuch; echo "rc=$?"'
  '{ y=2; typeset -n y; unset -n y; typeset -n y; } 2>&1 | sed "s|^[^ ]*: ||"; echo "rc=$?"'
  # A `{var}'"'"' redirection target that cannot be assigned is blamed on `exec'"'"' when
  # the redirection is exec'"'"'s own; on a compound command it is reported bare.
  '{ declare -n r; exec {r}>/dev/null; } 2>&1 | sed "s|^[^ ]*: ||"; echo "rc=$?"'
  '{ declare -n r; { echo hi; } {r}>/dev/null; } 2>&1 | sed "s|^[^ ]*: ||"; echo "rc=$?"'
  'exec {fd}>/dev/null; echo "ok=$((fd>2))"; exec {fd}>&-'
  # `${!ref[sub]}'"'"' on a NAMEREF indirects through that element of the target.
  # An element naming nothing is an invalid indirect expansion; `[@]'"'"'/`[*]'"'"' stay
  # the list-the-indices forms, and a plain variable subscripts to nothing.
  '{ declare -n foo=bar; echo ${!foo[2]}; } 2>&1 | sed "s|^[^ ]*: ||"'
  'declare -n foo=bar; bar=(a q c); q=HIT; echo "${!foo[1]}"'
  'declare -n foo=bar; bar=(x y z); echo "${!foo[@]}"'
  'foo=bar; echo "[${!foo[2]}]"'
  # Under `set -u'"'"' an unset element is named by the subscript as WRITTEN, not by
  # the index it evaluated to.
  '{ set -u; a=() k=; "${a[k]}"; } 2>&1 | sed "s|^[^ ]*: ||"'
  '{ set -u; a=(); "${a[0]}"; } 2>&1 | sed "s|^[^ ]*: ||"'
  '{ set -u; declare -A m; m=(); "${m[key]}"; } 2>&1 | sed "s|^[^ ]*: ||"'
  'set -u; a=(1 2); k=1; echo "${a[k]}"'
  # Resolving a nameref EVALUATES its subscript, so a `set -u'"'"' failure there is
  # reported against that variable -- once -- and not against the nameref.  An
  # associative subscript is a key, not arithmetic, so the nameref is blamed.
  '{ set -u; declare -n r="a[k]"; : "$r"; } 2>&1 | sed "s|^[^ ]*: ||"'
  '{ set -u; a=(1 2); declare -n r="a[k]"; : "$r"; } 2>&1 | sed "s|^[^ ]*: ||"'
  'set -u; a=(1 2); k=1; declare -n r="a[k]"; echo "$r"'
  '{ set -u; declare -A m; declare -n r="m[key]"; : "$r"; } 2>&1 | sed "s|^[^ ]*: ||"'
  'set -u; declare -A m=([key]=V); declare -n r="m[key]"; echo "$r"'
  '{ set -u; unset zz; : "$zz"; } 2>&1 | sed "s|^[^ ]*: ||"'
  # An assignment value made only of ordinary characters expands to itself and
  # skips the expansion pipeline.  These pin what must NOT take that path --
  # process substitution in particular, which has no `$'"'"' or quote to give it away.
  'v=<(echo hi); echo "${v%%[0-9]*}"'
  'v=plainvalue; echo "$v"'
  'x=9; v=$x-lit; echo "$v"'
  'v=~; [ "$v" = "$HOME" ] && echo tilde-ok'
  'v=a\ b; echo "[$v]"'
  # ...and what an append must still honour, whatever fast path it takes.
  '{ readonly s=a; s+=b; } 2>&1 | sed "s|^[^ ]*: ||"; echo "[$s]"'
  'declare -l s=A; s+=BC; echo "[$s]"'
  's=a; s+=b true; echo "[$s]"'
  'declare -a v=(1 2); v+=x; declare -p v'
  'PATH=/bin; PATH+=:/xyz; echo "$PATH"'
  'declare -n r=t; t=a; r+=b; echo "[$t]"'
  # With stdin closed, pipe() hands out fd 0 as a pipe end; the child fd dance
  # must not close the descriptor it just put in place (redir5.sub).
  'exec <&-; echo hi | cat; echo rc=$?'
  'exec <&-; echo a | cat | cat; echo rc=$?'
  'exec <&-; read abcde 2>&1 | grep -q "read error"; echo rc=$?'
  # read(2) failing outright (closed fd) is a reported error, not a quiet EOF.
  '{ exec <&-; read x; echo rc=$?; } 2>&1 | sed "s|^[^ ]*: ||"'
  # \u/\U escapes encode via the locale charset (bash u32cconv) in printf,
  # %b and echo -e; >= 0x80000000 encodes to nothing.
  'export LC_ALL=en_US.UTF-8; printf "\uff\n" | od -An -b | tr -s " "'
  'export LC_ALL=en_US.UTF-8; printf "%b\n" "\uff" | od -An -b | tr -s " "'
  'export LC_ALL=en_US.UTF-8; echo -e "\\u0152" | od -An -b | tr -s " "'
  'export LC_ALL=en_US.UTF-8; printf "[%s]\n" "$(printf "\Uffffffff")"'
  # %ls/%lc: width and precision count wide characters, space-padded.
  'export LC_ALL=en_US.UTF-8; V=ಇಳಿಕೆಗಳು; printf "%4.2ls|%-4.2ls|%4.2lc|\n" "$V" "$V" "$V"'
  # LC_NUMERIC follows LANG/LC_ALL, including after a temp-env teardown.
  'unset LC_ALL; export LANG=de_DE.UTF-8; printf "%.2f\n" 1; LANG=C printf "%.2f\n" 1; printf "%.2f\n" 1'
  # IFS splits on characters (a trail byte of € is not a delimiter)...
  'export LC_ALL=en_US.UTF-8; IFS=$'"'"'\254'"'"'; t="+$'"'"'\342\202\254'"'"'+"; set -- $t; echo $#'
  # ...but ${x##pat} drops to bytes when the pattern is invalid multibyte.
  'export LC_ALL=en_US.UTF-8; e=$'"'"'\342\202\254'"'"'; echo "${e##*$'"'"'\202'"'"'}" | od -An -b | tr -s " "'
  # HEREDOC_MAX (16): the 17th here-document on one command is fatal.
  'cat <<A <<A <<A <<A <<A <<A <<A <<A <<A <<A <<A <<A <<A <<A <<A <<A <<A; echo unreached'
  # ...but 16 on one command is fine.
  'cat <<A <<A <<A <<A <<A <<A <<A <<A <<A <<A <<A <<A <<A <<A <<A <<A >/dev/null
A
A
A
A
A
A
A
A
A
A
A
A
A
A
A
A
echo ok16'
  # printf %c with an empty/missing argument emits one NUL byte (not nothing).
  'printf "%c" "" | od -An -tx1 | tr -s " "'
  'printf "[%c]" "" "" | od -An -c | tr -s " "'
  # printf accepts (and ignores) the l length modifier on numeric/float convs;
  # %q stays the shell-quote conversion, not a length-modified `q'.
  'printf "%ld|%5ld|%lx|%lX|%lo|%lu\n" 42 42 255 255 64 42'
  'printf "%lf|%le|%lg\n" 3.14 3.14 3.14'
  'printf "%q %q %q\n" "a b" "it'"'"'s" "*.x"'
  # help NAME prints bash'"'"'s long-format body (not just the one-line summary).
  'help cd'
  'help printf'
  'help "[[ ... ]]"'
  'help -m read | grep -v version'
  'type --help; shift --help'
  # `variables' intentionally diverges from bash (it lists gnash'"'"'s own
  # $GNASH_* tunables), so compare only the bash-derived portion up to the
  # last shared entry, which must still match bash byte-for-byte.
  'help variables | sed -n "/^variables:/,/should be saved on the history list/p"'
  # Parameter-expansion validation (new-exp.tests).  Fold stderr and strip the
  # `NAME: line N: ` prefix so the diagnostic text is what is compared.
  'echo "${!1*}" 2>&1 | sed -E "s/.*line [0-9]+: //"'      # digit prefix listing
  'echo "${!@*}" 2>&1 | sed -E "s/.*line [0-9]+: //"'      # doubled special
  '_Qa=1; echo "${!_Q* }" 2>&1 | sed -E "s/.*line [0-9]+: //"'  # trailing junk
  'echo "${!@}"; echo "${!*}"'                             # valid: empty prefix
  'v=hi; echo "${v@}" 2>&1 | sed -E "s/.*line [0-9]+: //"' # empty transform op
  'v=hi; echo "${v@C}" 2>&1 | sed -E "s/.*line [0-9]+: //"' # unknown transform op
  'unset v; echo "[${v@C}]"'                               # unset: empty, no error
  'echo "${$((1))}" 2>&1 | sed -E "s/.*line [0-9]+: //"'   # arith in name position
  'echo "${$(echo x)}" 2>&1 | sed -E "s/.*line [0-9]+: //"' # comsub in name position
  'set -u; echo "${9}" 2>&1 | sed -E "s/.*line [0-9]+: //"; set -u; echo "$9" 2>&1 | sed -E "s/.*line [0-9]+: //"'
  'set a; echo "${@:1:$(($# - 2))}" 2>&1 | sed -E "s/.*line [0-9]+: //"'  # neg length raw text
  # Unquoted $@ in a QUOTED ${var±word} substitute keeps "$@" field structure:
  # one field per positional, literal word text glued to the first/last field.
  'f() { for a in "$@"; do echo "[$a]"; done; }; set "a b" c d; f "${1+$@}"'
  'f() { for a in "$@"; do echo "[$a]"; done; }; set abc def; f "${1+  $@  }"'
  'f() { for a in "$@"; do echo "[$a]"; done; }; set "a b" c; unset foo; f "${foo- x$@y }"'
  'f() { for a in "$@"; do echo "[$a]"; done; }; set "a b" c; unset foo; f "${foo-"$@" tail}"'
  'f() { for a in "$@"; do echo "[$a]"; done; }; unset foo; f "${foo-a"b  c"d}"; set --; f "${foo-x$@y}"'
  'f() { for a in "$@"; do echo "[$a]"; done; }; set -- "" x; unset foo; f "${foo-$@}"'
  # Dynamic specials: declare/export attributes never shadow the live value
  # (a seeded RANDOM sequence is deterministic), and unset kills the
  # specialness permanently -- later assignments store ordinary values.
  'RANDOM=42; echo "$RANDOM $RANDOM ${#RANDOM}"'
  'declare -i RANDOM=42; echo "$RANDOM $RANDOM"'
  'unset RANDOM; echo "[$RANDOM]"; RANDOM=7; echo "[$RANDOM]"'
  'unset SECONDS; SECONDS=5; echo "[$SECONDS]"'
  'unset LINENO; LINENO=999; echo "[$LINENO]"'
  'unset EPOCHSECONDS BASHPID BASH_SUBSHELL HISTCMD; echo "[$EPOCHSECONDS][$BASHPID][$BASH_SUBSHELL][$HISTCMD]"'
  # Backquotes in double quotes also unescape \" (the inner command sees real
  # quote syntax); unquoted backquotes and arithmetic context keep \" intact.
  'echo "`echo \"Hi there\"`"'
  'echo `echo \"Hi\"`'
  'echo "`echo \\\"x\\\"`"'
  'echo $((`echo \"1\"`+1)) 2>&1 | sed -E "s/.*line [0-9]+: //"'
  # Unquoted @-splats in an UNQUOTED ${var±word} substitute are space-joined
  # then IFS-split (bash posixexp4 quirk): one word under IFS=:, separate
  # fields under a null IFS, ordinary splitting under the default IFS.
  'f() { echo "n=$#"; for a; do echo "[$a]"; done; }; set -- " abc" "def ghi"; IFS=:; unset v; f ${v-$@}; f ${v-${@}}'
  'f() { echo "n=$#"; for a; do echo "[$a]"; done; }; a=("x y" z); IFS=:; unset v; f ${v-${a[@]}}'
  'f() { echo "n=$#"; for a; do echo "[$a]"; done; }; set -- "a b" "" c; IFS=; unset v; f ${v-$@}; f ${v-$*}'
  'f() { echo "n=$#"; for a; do echo "[$a]"; done; }; set -- "a b" "" c; unset v; f ${v-$@}; f ${v-"$@"}'
  # ${!prefix@}/${!prefix*} name listings carry $@/$* field semantics; only
  # visible (set) variables are listed, and an empty quoted @ list drops.
  'f() { echo "n=$#"; for a; do echo "[$a]"; done; }; _QA=1 _QB=2; IFS="-$IFS"; f "${!_Q*}"; f "${!_Q@}"; f ${!_Q*}; f "${!_Y@}"'
  'declare _QUNSET; _QA=1; echo "[${!_Q*}]"'
  # patsub_replacement: unquoted & in the replacement expands to the match,
  # \& is literal, quoted portions are inert, shopt -u restores literal &.
  's=abcdefg; echo ${s/abc/& }; echo "${s//?/& }"; echo ${s/abc/\& }; echo ${s/abc/"& "}; echo ${s/abc/\\& }'
  's=abcdefg; r="x&y"; echo ${s/abc/$r}; echo ${s/abc/"$r"}; r2="x\&y"; echo ${s/abc/$r2}'
  's=abcdefg; shopt -u patsub_replacement; echo ${s/abc/& }; echo ${s/abc/\&}; r="\\&"; echo ${s/abc/"$r"}'
  's=abcdefg; echo ${s/#abc/&-}; echo ${s/%efg/-&}'
  # @A/@a report full attributes in bash order; declared-but-unset variables
  # print attributes+name with no value (or nothing when attribute-free).
  'declare -lr V1; echo "[${V1@A}][${V1@a}]"; declare -alr V3; echo "[${V3@A}][${V3[@]@A}][${V3[@]@a}]"'
  'declare P; echo "[${P@A}][${P@a}]"; unset Z; echo "[${Z@A}]"; X=hi; echo "[${X@A}]"'
  'A2=(x); declare -r A2; echo "[${A2[@]@A}]"; b=(x y); declare -r b; echo "[${b[@]@a}]"'
  # Indirection composes with transforms and subscripts: ${!var@Q},
  # ${!name[0]}, ${!name[@]}OP (invalid joined name errors), and an
  # array-splat target keeps its field structure.
  'VAR2=zzz; var=VAR2; echo "${!var@Q}"; echo "${!var@U}"'
  'VAR4=(aaa bbb); varname=VAR4; echo "[${!varname[0]}]"; echo "[${!varname[@]}]"; echo "${!varname[@]@Q}"'
  'VAR4=(aaa bbb); echo ${!VAR4[@]@Q} 2>&1 | sed -E "s/.*line [0-9]+: //"; echo "[${!VAR4[@]}]"'
  'VAR5=(aaa bbb); v="VAR5[@]"; f(){ echo "n=$#"; for a; do echo "[$a]"; done; }; f "${!v@Q}"; f "${!v}"; echo "${!v%b}"'
  # Pattern substitution honors nocasematch (removal and case-mod do not).
  's=abcd; shopt -s nocasematch; echo ${s//A/z}; echo ${s//BC/x}; echo ${s//[BC]/x}; echo ${s//[bC]/x}'
  's=ABCdef; shopt -s nocasematch; echo "[${s#abc}]"; echo "[${s%DEF}]"; echo "[${s^^[ab]}]"; echo ${s/def/&!}'
  # set -u vs @a/@A: an array with ANY element is set for the transforms;
  # an empty array errors (naming !name through indirection), and @A on an
  # elem-0-unset array prints attributes+name with no value.
  'declare -a foo=([1]=one); set -u; echo "[${foo@a}][${foo@A}]"; bar=foo; echo "[${!bar@A}]"'
  'set -u; declare -ia foo=(); echo "[${foo@a}]" 2>&1 | sed -E "s/.*line [0-9]+: //"'
  'set -u; declare -ia foo=(); bar=foo; echo "[${!bar@a}]" 2>&1 | sed -E "s/.*line [0-9]+: //"'
  'set -u; arr=(zero one); name="arr[@]"; f(){ echo n=$#; }; f "${!name}"'
  # ${@@A}/${*@A} reconstruct the positional parameters as a set command.
  'set -- ab "cd ef" "" gh; printf "<%s> " "${@@A}"; echo; printf "<%s> " "${*@A}"; echo'
  'set --; printf "<%s> " "${@@A}"; echo'
  # @P prompt decoding: octal escapes, promptvars $-expansion with
  # escape-produced characters literal, \[ \] markers only under editing,
  # posix ! history number.
  'HOST=host L=3; x="\[\]${HOST}($L)\041\$ "; recho "${x@P}"'
  'foo=zz; x="\\\$foo"; recho "${x@P}"; z="\`echo cs\`"; recho "${z@P}"; shopt -u promptvars; recho "${x@P}"'
  'x="\[\001\]ok"; recho "${x@P}"; set -o emacs; recho "${x@P}"'
  'set -o posix; x="!x\!y!!z"; recho "${x@P}"'
  # Indirection completeness: unset positionals default, invalid targets
  # error, special-parameter targets keep $@ fields, digit inames re-dispatch.
  'z=ZV; echo "[${!9:-$z}]"; echo "[${!9-$z}]"; unset x; echo "[${!x-$z}]" 2>&1 | sed -E "s/.*line [0-9]+: //"'
  'v=bad-var; echo "${!v}" 2>&1 | sed -E "s/.*line [0-9]+: //"'
  'foo=@; set -- a "b c" d; f(){ echo n=$#; for a; do echo "[$a]"; done; }; f ${!foo}; f "${!foo}"'
  'arr_1=(x "y z"); set -- "arr_1[@]"; a=("${!1}"); printf "<%s>" "${a[@]}"; echo'
  # Patsub parsing: anchors and // are exclusive, the // delimiter scan skips
  # the first char, empty values get one match attempt.
  'v=(abcde abcfg); echo "${v[*]//#abc/foo}"; echo "${v[*]/#abc/foo}"; a=/a; echo "/${a///a/}"; b=x/y; echo "${b////-}"'
  'var=; echo "[${var/#/x}][${var/\*/x}][${var//\*/x}]"'
  # Process substitution is live inside an unquoted substitute word.
  'foo=; cat ${foo:-<(echo a)}'
  # ${N=word} on positionals/specials errors; ${var[@]:off} on a scalar is
  # the string slice; escaped slashes in // patterns keep working.
  'set -- a; echo ${2="x"} 2>&1 | sed -E "s/.*: .[$]/\$/"; echo "[${1=z}]"'
  'var=blah; echo "[${var[@]:3}]" "[${var[@]:1:2}]" "[${var[*]:3}]"'
  'p=(/usr/bin /bin); echo "${p[@]//\//^}"'
)

fails=0
for s in "${scripts[@]}"; do
  # Run the script via -c (stdin left as /dev/null so a stray `read' can't
  # hang).  NOTE: the script must be passed as an argument, not piped to stdin;
  # piping while redirecting stdin from /dev/null makes the shell read nothing.
  g_out=$("$gnash" -c "$s" </dev/null 2>/dev/null); g_rc=$?
  b_out=$("$bash"  -c "$s" </dev/null 2>/dev/null); b_rc=$?
  if [ "$g_out" != "$b_out" ] || [ "$g_rc" != "$b_rc" ]; then
    echo "MISMATCH: $s" >&2
    echo "  gnash (rc=$g_rc): $g_out" >&2
    echo "  bash  (rc=$b_rc): $b_out" >&2
    fails=$((fails + 1))
  fi
done

if [ $fails -eq 0 ]; then
  echo "run_diff: all ${#scripts[@]} scripts match bash"
  exit 0
fi
echo "run_diff: $fails mismatch(es)" >&2
exit 1
