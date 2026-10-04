#!/usr/bin/env bash
# Record intended (plugin) and actual (compositor) frame timing together.
#   jointest.sh <label> [latch_us] [pace_mode]
T="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
bash $T/recenter.sh >/dev/null   # known mid-map view: tests must not walk the camera off the map
L="${1:-j}"
[ -n "${2:-}" ] && bash $T/dfr.sh "smoothpan pace latch $2" >/dev/null
bash $T/dfr.sh "smoothpan pace ${3:-auto}" >/dev/null
sleep 2
bash $T/dfr.sh "smoothpan pace record 300 $L" >/dev/null
bash $T/dflua.sh "dfhack.timeout(2,'frames',function() dfhack.run_command('smoothpan','testpan','1','0','1500') end); dfhack.timeout(170,'frames',function() dfhack.run_command('smoothpan','testpan','-1','0','1500') end)" >/dev/null
timeout 12 $T/vblmsc 0x320000c 1700 1500 > $T/join_$L.txt
for i in $(seq 1 10); do bash $T/dfr.sh "smoothpan pace" | grep -q 'rec_left=0' && break; sleep 1; done
echo "[$L latch=${2:-cur}]"
python3 $T/joinshow.py "$L" $T/join_$L.txt | sed 's/^/  /'
