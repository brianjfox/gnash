#!/bin/bash
shopt -s extglob
for w in foo.txt bar.log baz README abc123; do
  case $w in
    *.txt)   echo "$w: text" ;;
    *.@(log|out)) echo "$w: logish" ;;
    [A-Z]*)  echo "$w: upper" ;;
    +([a-z])+([0-9])) echo "$w: alnum" ;;
    *)       echo "$w: other" ;;
  esac
done
[[ abc123 =~ ^([a-z]+)([0-9]+)$ ]] && echo "re: ${BASH_REMATCH[1]}/${BASH_REMATCH[2]}"
