#!/bin/bash
# Multibyte (UTF-8) string handling: length, substrings, case, patterns.
export LC_ALL=en_US.UTF-8
x=café
echo "len=${#x} up=${x^^} low=${x,,} sub=${x:1:2} last=${x: -1}"
echo "strip=${x%?} head=${x#?} ext=${x%.*}"
[[ $x == c??é ]] && echo "match-q ok"
[[ é == ? ]] && echo "one-char ok"
[[ é == [[:alpha:]] ]] && echo "class ok"
case résumé in r?sum?) echo "case ok" ;; *) echo "case BAD" ;; esac
declare -u U=münchen; declare -l L=BJÖRK; echo "$U $L"
