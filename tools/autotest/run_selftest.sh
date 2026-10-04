#!/usr/bin/env bash
# Run a smoothpan selftest at a given graphical fps cap and print the verdict.
#   run_selftest.sh <gfps> <label>
set -uo pipefail
D="/home/jospeh/.local/share/Steam/steamapps/common/DFHack"
SPD="/home/jospeh/.local/share/Steam/steamapps/common/Dwarf Fortress/dfhack-config/smoothpan"
GFPS="${1:-60}"
LABEL="${2:-run}"
cd "$D" || exit 1

./dfhack-run lua "df.global.enabler.gfps = $GFPS" >/dev/null 2>&1
sleep 1
echo "=== gfps set to: $(./dfhack-run lua 'print(df.global.enabler.gfps)' 2>&1 | tr -d '\033[0m')"

./dfhack-run smoothpan selftest >/dev/null 2>&1
for i in $(seq 1 60); do
    sleep 2
    if ! ./dfhack-run smoothpan selftest status 2>&1 | grep -qi running; then break; fi
done
./dfhack-run smoothpan selftest status 2>&1 | head -3

echo "--- VERDICT ($LABEL @ ${GFPS}fps) ---"
sed -n '/== VERDICT ==/,/RESULT/p' "$SPD/smoothpan_selftest.txt" | grep -E '^(OK|WARN|FAIL|INFO RESULT|INFO parity)'

cp -f "$SPD/smoothpan_selftest.txt" "$T/selftest_${LABEL}_${GFPS}.txt"
