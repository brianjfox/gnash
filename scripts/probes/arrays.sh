#!/bin/bash
a=(one two three four)
echo "${a[0]} ${a[-1]} ${#a[@]} ${a[@]:1:2}"
echo "${!a[@]}"
declare -A m=([x]=1 [y]=2 [z]=3)
for k in x y z; do printf '%s=%s ' "$k" "${m[$k]}"; done; echo
a+=(five); echo "${a[@]}"
echo "${a[*]}"
b=("${a[@]}"); echo "${#b[@]}"
