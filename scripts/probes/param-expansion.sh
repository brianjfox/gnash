#!/bin/bash
v=HelloWorld
echo "${v,,} ${v^^} ${v:2:3} ${v: -3} ${#v}"
echo "${v/l/L} ${v//l/L} ${v#Hel} ${v%rld}"
x=; echo "[${x:-def}] [${x:+set}] [${x-D}]"
a=foo.bar.baz; echo "${a%.*} ${a##*.}"
printf '%s\n' "${v@Q}" "${v@U}" "${v@L}"
unset u; echo "${u:=assigned} u=$u"
