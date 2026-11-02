#!/bin/bash
# Drives TotK Recomp by keyboard: hold KEY SECS / tap KEY / shot NAME
export DISPLAY=:0
S=${S:-$(mktemp -d)}
W=$(xdotool search --name '^TotK Recomp$' | head -1)
xdotool windowactivate "$W" 2>/dev/null; xdotool windowfocus "$W" 2>/dev/null; sleep 0.7
hold() { xdotool keydown $1; sleep $2; xdotool keyup $1; }
tap() { hold "$1" 0.12; sleep 0.4; }
shot() { spectacle -b -n -f -o "$S/$1.png" 2>/dev/null; }
eval "$@"
