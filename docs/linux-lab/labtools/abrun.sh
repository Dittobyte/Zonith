#!/bin/bash
# abrun.sh EXE_A EXE_B ROUNDS: interleaved fresh-launch measurements (A B A B ...). Run from labtools with S set.
export S=${S:-$(cd "$(dirname "$0")" && pwd)}
G=~/Games/TotK-Recomp
closegame() { pkill -x totk.exe; for i in $(seq 30); do { pgrep -x totk.exe || pgrep -x wineserver; } >/dev/null || break; sleep 2; done; }
for r in $(seq 1 $3); do
  for x in $1 $2; do
    closegame; cp $G/totk.exe.$x $G/totk.exe
    steam steam://rungameid/9747207482626801664 >/dev/null 2>&1 &
    sleep 30
    for i in $(seq 1 60); do grep -q "perf: frame 400: interval\|FATAL" $G/logs/totk.log && break; sleep 5; done
    grep -q FATAL $G/logs/totk.log && { echo "$x FATAL"; continue; }
    bash $S/drive.sh "shot title_$x$r"
    bash $S/load.sh >/dev/null
    bash $S/measure.sh "${x}_r$r" | tail -2
  done
done
closegame
