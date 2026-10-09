#!/bin/sh
# tour.sh MONSOCK PREFIX "keys..."  — send keys through the monitor; "shot" takes a screenshot
S=$1; P=$2; shift 2; n=0
for k in $*; do
  case $k in
    shot) n=$((n+1)); echo "screendump $P-$n.ppm" | nc -U -w1 $S >/dev/null; sleep 0.3;;
    wait) sleep 2;;
    ret) echo "sendkey ret 400" | nc -U -w1 $S >/dev/null; sleep 1;;
    w) echo "sendkey w 400" | nc -U -w1 $S >/dev/null; sleep 1;;
    *) echo "sendkey $k" | nc -U -w1 $S >/dev/null; sleep 0.6;;
  esac
done
