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
  # ulimit reports a single resource as a bare value
  'ulimit -n; ulimit -c'
  # set -u: defaulting operators handle an unset variable (no nounset error)
  'set -u; echo "${UNSET:-def}" "${UNSET-d2}" "${UNSET:+a}" "${OTHER:=x}"; echo "$OTHER"'
  'set -u; v=hi; echo "${v:-nope}${v:+yes}"'
  # set -u: a genuine unbound reference (and ${x?}) is fatal with status 127
  'set -u; echo pre; echo "$UNSET"; echo post'
  'echo pre; echo "${UNSET?boom}"; echo post'
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
