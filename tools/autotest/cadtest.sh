#!/usr/bin/env bash
# Measure on-screen cadence during a pan for a given pacing mode.
#   cadtest.sh <off|auto|N>
T="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
bash $T/recenter.sh >/dev/null   # known mid-map view: tests must not walk the camera off the map
bash $T/dfr.sh "smoothpan pace $1" >/dev/null
sleep 2
bash $T/dflua.sh "dfhack.timeout(3,'frames',function() dfhack.run_command('smoothpan','testpan','1','0','1500') end); dfhack.timeout(170,'frames',function() dfhack.run_command('smoothpan','testpan','-1','0','1500') end)" >/dev/null
sleep 0.3
echo "[pace $1]"
python3 $T/cadence.py 2.5 180 | sed 's/^/  /'
