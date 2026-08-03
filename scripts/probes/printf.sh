#!/bin/bash
printf '[%5d][%-5d][%05d]\n' 42 42 42
printf '[%8.3f][%+d][%x][%o]\n' 3.14159 7 255 64
printf '%b\n' 'a\tb\nc'
printf '%q %q\n' "a b" "it's"
printf '%d %d %d\n' "'A" 0x10 010
printf '%s|' a b c; echo
printf '%.2s\n' abcdef
