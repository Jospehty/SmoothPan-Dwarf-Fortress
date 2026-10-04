#!/usr/bin/env bash
# Build is assumed done.  Swap the new binary into the running game safely:
# disable (hooks come off on the render thread), unload, copy, load, enable.
T="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
bash $T/dfr.sh "disable smoothpan" >/dev/null
sleep 0.3
bash $T/dfr.sh "unload smoothpan" >/dev/null
bash $T/install.sh
bash $T/dfr.sh "load smoothpan" >/dev/null
bash $T/dfr.sh "enable smoothpan" | head -1
sleep 0.5
bash $T/dfr.sh "smoothpan diag" | grep -E 'hooks: platform|compositor=|watch=' | cut -c1-140
