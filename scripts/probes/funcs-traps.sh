#!/bin/bash
fac() { local n=$1; (( n<=1 )) && { echo 1; return; }; local r; r=$(fac $((n-1))); echo $((n*r)); }
echo "5!=$(fac 5)"
trap 'echo EXIT-trap' EXIT
greet() { local name=${1:-world}; printf 'hi %s\n' "$name"; }
greet; greet gnash
x=outer; f() { local x=inner; echo "$x"; }; f; echo "$x"
