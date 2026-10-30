#!/bin/bash
# measure.sh TAG: standing 30 s (perf in the middle) + walking loop, then summary. Game must be in the world at the spot.
S=${S:-$(mktemp -d)}
L=~/Games/TotK-Recomp/logs/totk.log
T=$1
fr() { grep "perf: frame .*: interval" $L | tail -1 | sed 's/.*perf: frame \([0-9]*\):.*/\1/'; }
bash $S/drive.sh "shot ${T}_pre"
a=$(fr); sleep 8
P=$(pgrep -x totk.exe); perf record -F 1999 -g -p $P -o $S/$T.data -- sleep 12 >/dev/null 2>&1
sleep 10; b=$(fr)
bash $S/drive.sh '
for r in 1 2; do
 xdotool keydown w; sleep 6; xdotool keyup w; xdotool keydown s; sleep 6; xdotool keyup s
 xdotool keydown a; sleep 4; xdotool keyup a; xdotool keydown d; sleep 4; xdotool keyup d
 hold Right 3; hold Left 3
done'
c=$(fr); bash $S/drive.sh "shot ${T}_post"
echo "$T stand $a-$b walk $b-$c" | tee -a $S/ranges.txt
python3 -I $S/summ.py $L $a $b stand
python3 -I $S/summ.py $L $b $c walk
