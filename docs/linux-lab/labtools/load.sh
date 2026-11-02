#!/bin/bash
# load.sh: from the title menu, Continue -> top save -> Load, then wait until 15 s into the world.
S=${S:-$(mktemp -d)}
L=~/Games/TotK-Recomp/logs/totk.log
bash $S/drive.sh 'tap e; sleep 3; tap e; sleep 2; shot load_confirm; tap e'
for i in $(seq 1 50); do
  w=$(grep "perf: frame .*interval" $L | tail -1 | grep -o "world [0-9.]* s" | grep -o "[0-9.]*")
  [ -n "$w" ] && awk "BEGIN{exit !($w>15)}" && break; sleep 5
done
bash $S/drive.sh 'shot loaded'
grep "perf: frame .*interval" $L | tail -1 | grep -o "world [-0-9.]* s"
