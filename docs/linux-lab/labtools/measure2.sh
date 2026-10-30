#!/bin/bash
# measure2.sh TAG: like measure.sh, plus after the standing perf: render-thread asleep % and the other threads' page faults/s (5 s).
S=${S:-$(cd "$(dirname "$0")" && pwd)}
L=~/Games/TotK-Recomp/logs/totk.log
T=$1
fr() { grep "perf: frame .*: interval" $L | tail -1 | sed 's/.*perf: frame \([0-9]*\):.*/\1/'; }
bash $S/drive.sh "shot ${T}_pre"
a=$(fr); sleep 8
P=$(pgrep -x totk.exe); perf record -F 1999 -g -p $P -o $S/$T.data -- sleep 12 >/dev/null 2>&1
R=$(perf report -i $S/$T.data --children --sort pid,sym --stdio -g none 2>/dev/null | grep "thunk64_vkQueueSubmit2$" | sort -rn | head -1 | awk '{print $3}' | cut -d: -f1)
flt() { for t in /proc/$P/task/*; do [ "${t##*/}" = "$R" ] || awk '{s+=$10} END{print s+0}' $t/stat 2>/dev/null; done | awk '{s+=$1} END{print s}'; }
read on0 rq0 x < /proc/$P/task/$R/schedstat; f0=$(flt); t0=$(date +%s.%N)
sleep 5
read on1 rq1 x < /proc/$P/task/$R/schedstat; f1=$(flt); t1=$(date +%s.%N)
python3 -c "w=$t1-$t0; print(f'$T render tid $R: on-CPU {100*($on1-$on0)/1e9/w:.1f}%, runqueue {100*($rq1-$rq0)/1e9/w:.1f}%, asleep {100-100*($on1-$on0+$rq1-$rq0)/1e9/w:.1f}% | other threads minor faults {($f1-$f0)/w:.0f}/s')" | tee -a $S/extra.txt
b=$(fr)
bash $S/drive.sh '
for r in 1 2; do
 xdotool keydown w; sleep 6; xdotool keyup w; xdotool keydown s; sleep 6; xdotool keyup s
 xdotool keydown a; sleep 4; xdotool keyup a; xdotool keydown d; sleep 4; xdotool keyup d
 hold Right 3; hold Left 3
done'
c=$(fr); bash $S/drive.sh "shot ${T}_post"
echo "$T stand $a-$b walk $b-$c" | tee -a $S/ranges.txt
python3 -I $S/summ.py $L $a $b "$T stand"
python3 -I $S/summ.py $L $b $c "$T walk"
