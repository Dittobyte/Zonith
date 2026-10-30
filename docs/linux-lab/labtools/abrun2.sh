#!/bin/bash
# abrun2.sh "EXE1 EXE2 ..." ROUNDS: interleaved fresh launches with measure2.sh.
export S=${S:-$(cd "$(dirname "$0")" && pwd)}
G=~/Games/TotK-Recomp
closegame() { pkill -x totk.exe; for i in $(seq 30); do { pgrep -x totk.exe || pgrep -x wineserver; } >/dev/null || break; sleep 2; done; }
EXES="$1"; for r in $(seq 1 $2); do
  for x in $EXES; do
    closegame; cp $G/totk.exe.$x $G/totk.exe
    rm -rf $G/profile/save && tar xzf $G/save-backup-2026-10-30-1433.tgz -C $G/profile   # same save every run (autosaves move the list)
    steam steam://rungameid/9747207482626801664 >/dev/null 2>&1 &
    sleep 30
    for i in $(seq 1 60); do grep -q "perf: frame 400: interval\|FATAL" $G/logs/totk.log && break; sleep 5; done
    grep -q FATAL $G/logs/totk.log && { echo "$x FATAL"; continue; }
    bash $S/drive.sh "shot title_$x$r"
    bash $S/load.sh >/dev/null
    bash $S/measure2.sh "${x}_r$r" | tail -2
  done
done
closegame
