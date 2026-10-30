#!/bin/sh
# Steam launch wrapper: Launch Options = /home/deck/Games/TotK-Recomp/run.sh %command%
# Settings live in launch.env next to this script, so they can change without editing Steam.
D=/home/deck/Games/TotK-Recomp
set -a; [ -f "$D/launch.env" ] && . "$D/launch.env"; set +a
exec "$@" $TOTK_ARGS
