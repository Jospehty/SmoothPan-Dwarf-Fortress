#!/usr/bin/env bash
# Pan cadence at a given graphics cap and pacing mode, no pixel readback.
#   capprobe.sh <gfps> <pace mode: auto|N> <label>
T="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
bash $T/dflua.sh "df.global.enabler.gfps=$1" >/dev/null
bash $T/dfr.sh "smoothpan pace $2" >/dev/null
sleep 1
PANREC_NOPIX=nopix bash $T/panrec.sh "$3" 500 "pan1,0,1500 w100 pan-1,0,1500" >/dev/null 2>&1
python3 $T/refhist.py "$3"
bash $T/dfr.sh "smoothpan pace" | grep -o 'every=[0-9]*\|busy_p90=[0-9]*us\|render_ema=[0-9]*us\|late=[0-9]*\|held=[0-9]*' | tr '\n' ' '; echo
