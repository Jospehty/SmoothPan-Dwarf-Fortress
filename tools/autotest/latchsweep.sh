#!/usr/bin/env bash
# Misses per present-margin setting.  usage: latchsweep.sh <us> [<us> ...]
T="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
for L in "$@"; do
    bash $T/jointest.sh "lat$L" "$L"
    python3 $T/joindetail.py "lat$L" "$T/join_lat$L.txt" 2>/dev/null | grep -E 'miss times' | sed 's/^/  /'
    bash $T/dfr.sh "smoothpan pace" | grep -oE 'every=[0-9]+|busy_p90=[0-9]+us|late=[0-9]+' | tr '\n' ' ' | sed 's/^/  /'; echo
done
