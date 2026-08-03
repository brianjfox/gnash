#!/bin/bash
echo {1..5} {a..e} {01..03} {1..10..2}
echo pre{X,Y,Z}post
echo "$(echo nested $(echo sub))"
echo 'single '"double $HOME-less"' back'
v='a b c'; for w in $v; do printf '<%s>' "$w"; done; echo
echo "${v}" and $v | tr ' ' '_'
