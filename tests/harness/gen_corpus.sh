#!/usr/bin/env bash
# Copyright (c) 2026 Brian J. Fox
# Licensed under GPLv2 with the GPLv2-AI Exception.
#
# gen_corpus.sh  >  corpus.nul
#
# Emit a large, self-authored corpus of small, side-effect-free shell snippets,
# NUL-delimited (snippets may themselves contain newlines, e.g. here-docs), that
# systematically exercise bash language features across a range of inputs.
# Because every snippet is generated here (not harvested), it is safe to EXECUTE
# under the differential harness.  The point is breadth: parametrized templates
# over many inputs surface divergences a fixed 176-script list cannot.
set -u
emit() { printf '%s\0' "$1"; }

# --- arithmetic: operators x a representative operand sample ------------------
# (a balanced sample -- not a full cross product, which would swamp the corpus)
for pair in '7 3' '10 -2' '0 2' '255 5' '-4 5' '1 10'; do
  set -- $pair; a=$1; b=$2
  for op in '+' '-' '*' '/' '%' '**' '<<' '>>' '&' '|' '^' '<' '>' '==' '!=' '<=' '>='; do
    emit "echo \$(( $a $op $b ))"
  done
done
for e in '2+3*4' '(2+3)*4' '10%3+1' '1<<10' '~0' '!0' '!5' '5>3?100:200' \
         '3>2?1<0?9:8:7' '0x1f + 010 + 2#101' '5,6,7' 'a=3,a*2' \
         '1&&0||1' '0||0' '-(-5)' '+ +7' '2**2**3'; do
  emit "echo \$(( $e ))"
done
emit 'x=10; echo $((x++)) $((++x)) $((x--)) $((--x)) $x'
emit 'x=5; echo $((x+=3)) $((x-=1)) $((x*=2)) $((x/=3)) $x'
emit 'x=12; echo $((x&=10)) $((x|=1)) $((x^=3)) $((x<<=1)) $((x>>=2)) $x'

# --- parameter expansion: forms x set/unset/empty -----------------------------
for setup in 'v=hello' 'v=' 'unset v'; do
  for pe in '${v:-DEF}' '${v-DEF}' '${v:=ASG}' '${v:+ALT}' '${v+ALT}' \
            '${#v}' '${v:1}' '${v:1:3}' '${v: -2}' '${v:(-3):2}' \
            '${v^^}' '${v,,}' '${v^} ${v~}' '${v/l/L}' '${v//l/L}' \
            '${v/#he/HE}' '${v/%lo/LO}' '${v#h}' '${v##*l}' '${v%o}' '${v%%l*}'; do
    emit "$setup; echo \"$pe\""
  done
done
emit 'p=/a/b/c/file.tar.gz; echo ${p##*/} ${p%/*} ${p##*.} ${p%.*} ${p#*.}'
emit 'x=abcABCabc; echo "${x/[a-c]/_}" "${x//[a-c]/_}" "${x//[[:upper:]]/-}"'
emit 's=aXbXcXd; echo "${s/X/-}" "${s//X/-}" "${s/%d/D}"'

# --- indirection / prefix listing / length ------------------------------------
emit 'a=b; b=c; c=deep; echo "${!a}" "${!!}" 2>/dev/null; echo "${!a}"'
emit 'foo1=x; foo2=y; foobar=z; echo ${!foo*} | tr " " "\n" | sort | tr "\n" " "; echo'
emit 'set -- w x y z; echo "$# ${!#} ${@:2:2} ${@: -1}"'

# --- arrays: indexed & associative --------------------------------------------
emit 'a=(one two three four); echo "${a[0]}|${a[-1]}|${#a[@]}|${a[@]:1:2}"'
emit 'a=(1 2 3); a+=(4 5); a[10]=X; echo "${!a[@]}"; echo "${a[@]}"'
emit 'a=(alpha beta gamma); echo "${a[@]^^}" "${a[@]:0:1}" "${a[*]/a/A}"'
emit 'declare -A m=([x]=1 [y]=2 [z]=3); echo "${#m[@]}"; for k in x y z; do printf "%s=%s " "$k" "${m[$k]}"; done; echo'
emit 'declare -A m=([a b]="c d"); echo "[${m[a b]}]"'
emit 'a=(); echo "${#a[@]} ${a[@]:-empty}"; a[5]=hi; echo "${#a[@]} ${!a[@]}"'

# --- brace expansion ----------------------------------------------------------
for be in '{a,b,c}' '{1..5}' '{5..1}' '{1..10..2}' '{a..e}' '{a..e..2}' \
          'x{1,2}y' '{1,2}{3,4}' '{{a,b},c}' 'pre{A..C}post' '{01..05}' \
          '{-3..3}' 'file{1..3}.txt' '{a,b}{1..2}{x,y}'; do
  emit "echo $be"
done

# --- quoting & word splitting -------------------------------------------------
emit 'v="a  b   c"; for w in $v; do printf "[%s]" "$w"; done; echo; for w in "$v"; do printf "[%s]" "$w"; done; echo'
emit 'IFS=,; v="a,,b,c,"; for w in $v; do printf "[%s]" "$w"; done; echo'
emit 'set -- "" a "" b; printf "<%s>" "$@"; echo " n=$#"'
emit "echo 'single \$x' \"double \$HOME\" 2>/dev/null | head -c0; x=Q; echo 'lit\$x' \"exp\$x\""
emit 'printf "%s\n" "tab\there" | cat -v; printf "a\tb\n" | cat -v'

# --- printf breadth -----------------------------------------------------------
for fmt in '%d' '%5d' '%-5d|' '%05d' '%+d' '%x' '%X' '%o' '%e' '%g' '%c'; do
  emit "printf '$fmt\\n' 65"
done
emit 'printf "%s|%q|%b\n" "a b" "a b" "a\tb"'
emit 'printf "%d %d %d\n" 0x1f 010 42'
emit 'printf "%.3f|%8.2f|%-8.2f|\n" 3.14159 2.5 2.5'
emit 'printf "%b" "A\101\x42\n"'
emit 'printf "%(%Y)T\n" 0 2>/dev/null || echo notime'
emit 'printf "%s\n" a b c d'
emit 'printf "%s-%s\n" a b c d e'

# --- control flow combinations ------------------------------------------------
emit 'for i in 1 2 3; do for j in a b; do printf "%s%s " "$i" "$j"; done; done; echo'
emit 'i=0; while ((i<5)); do ((i++)); ((i==3)) && continue; printf "%d " "$i"; done; echo'
emit 'for i in 1 2 3 4 5; do ((i>3)) && break; printf "%d " "$i"; done; echo'
emit 'n=7; case $n in [0-4]) echo low;; [5-9]) echo high;; *) echo other;; esac'
emit 'x=cat; case $x in cat|dog) echo pet;; *) echo wild;; esac'
emit 'select_test() { local x=$1; if ((x>0)); then echo pos; elif ((x<0)); then echo neg; else echo zero; fi; }; select_test 5; select_test -3; select_test 0'
emit 'until [ $((RANDOM)) -ge 0 ]; do echo loop; done; echo done'
emit 'c=0; for ((i=0;i<100;i++)); do ((i%7==0)) && ((c++)); done; echo $c'

# --- functions, locals, recursion ---------------------------------------------
emit 'fib(){ local n=$1; ((n<2)) && { echo $n; return; }; echo $(( $(fib $((n-1))) + $(fib $((n-2))) )); }; fib 10'
emit 'fact(){ local n=$1 r=1; while ((n>1)); do ((r*=n,n--)); done; echo $r; }; fact 6'
emit 'f(){ local x=inner; g(){ echo "$x"; }; g; }; x=outer; f'
emit 'count(){ echo $#; }; count a b c; count; count "a b" c'

# --- test / [[ ]] conditionals ------------------------------------------------
for t in '1 -eq 1' '1 -ne 2' '2 -gt 1' '1 -lt 2' 'abc = abc' 'abc != xyz' \
         '-z ""' '-n x' 'a < b' 'z > a'; do
  emit "[[ $t ]] && echo T || echo F"
done
emit '[[ hello == h*o ]] && echo glob; [[ hello == h?llo ]] && echo q; [[ hello != x* ]] && echo neq'
emit '[[ foobar =~ o+b ]] && echo "${BASH_REMATCH[0]}"; [[ 2024 =~ ([0-9]{2})([0-9]{2}) ]] && echo "${BASH_REMATCH[1]}-${BASH_REMATCH[2]}"'
emit '[[ -e / && -d / && ! -f / ]] && echo rootdir'

# --- command substitution nesting ---------------------------------------------
emit 'echo "$(echo $(echo $(echo deep)))"'
emit 'x=$(printf "%s\n" a b c | wc -l); echo "lines=$((x))"'
emit 'echo "today has $(echo 24) hours and $(( 24*60 )) minutes"'
emit 'v=$(echo one; echo two); echo "$v" | tr "\n" ,; echo'

# --- here-docs / here-strings -------------------------------------------------
emit 'cat <<EOF
line one
line two
EOF'
emit 'x=world; cat <<EOF
hello $x
EOF'
emit 'cat <<"EOF"
no $expansion here
EOF'
emit 'cat <<-EOF
	tabbed
	indented
	EOF'
emit 'wc -w <<< "one two three four" | tr -d " "'
emit 'tr a-z A-Z <<< "shout"'
