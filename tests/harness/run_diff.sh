#!/usr/bin/env bash
# run_diff.sh GNASH BASH
#
# Differential execution: run each script under gnash and bash, comparing
# stdout and exit status.  These scripts are all within gnash's implemented
# feature set, so they must match exactly.  Exit 0 if all agree.
set -u

gnash=${1:?usage: run_diff.sh GNASH BASH}
bash=${2:?usage: run_diff.sh GNASH BASH}

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
)

fails=0
for s in "${scripts[@]}"; do
  g_out=$(printf '%s' "$s" | "$gnash" </dev/null 2>/dev/null); g_rc=$?
  b_out=$(printf '%s' "$s" | "$bash"  </dev/null 2>/dev/null); b_rc=$?
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
