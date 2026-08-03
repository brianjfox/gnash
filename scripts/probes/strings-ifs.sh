#!/bin/bash
IFS=, read -ra parts <<< "a,b,c,d"
echo "${#parts[@]}: ${parts[*]}"
s="  trim me  "; echo "[${s#"${s%%[![:space:]]*}"}]"
data="k1=v1;k2=v2"; IFS=';' read -ra kv <<< "$data"
for p in "${kv[@]}"; do echo "$p"; done
set -- alpha beta gamma; echo "$# $1 $3 $*"; shift 2; echo "$# $1"
