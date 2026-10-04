#!/usr/bin/env bash
# Full regression suite at a given graphics cap.  Every scenario drives the real
# input paths from inside the game loop and is judged from pixel-truth metrics.
#   suite.sh <gfps> <tag>
T="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
G="${1:-180}"; TAG="${2:-run}"
bash $T/dflua.sh "df.global.enabler.gfps=$G" >/dev/null
echo "================ SUITE $TAG @ ${G} fps ================"
echo "--- selftest"
bash $T/run_selftest.sh "$G" "st_${TAG}_$G" 2>&1 | grep -E 'RESULT|parity|compositor:' | sed 's/^/  /'
echo "--- Z1 single notches"
bash $T/zoomrec2.sh "z1_${TAG}_$G" 500 "out1 w100 in1 w100 out1 w100 in1 w100" | grep -E 'black|glide|corner' | sed 's/^/  /'
echo "--- Z2 3-notch flicks"
bash $T/zoomrec2.sh "z2_${TAG}_$G" 450 "out3 w160 in3 w160" | grep -E 'black|glide|corner' | sed 's/^/  /'
echo "--- Z3 rapid notch stream"
bash $T/zoomrec2.sh "z3_${TAG}_$G" 500 "out1 w6 out1 w6 out1 w120 in1 w6 in1 w6 in1 w6 out1 w150" | grep -E 'black|glide|corner' | sed 's/^/  /'
echo "--- P1 pan sweep"
bash $T/panrec.sh "p1_${TAG}_$G" 700 "pan1,0,1200 w140 pan0,1,700 w100 pan-1,0,1200 w140 pan0,-1,700" | grep -E 'black|pan |pixels|edges' | sed 's/^/  /'
echo "--- ZP zoom while panning"
bash $T/panrec.sh "zp_${TAG}_$G" 600 "pan1,0,2000 w30 out1 w60 in1 w60 out2 w80 in2" | grep -E 'black|pan |pixels|edges' | sed 's/^/  /'
python3 $T/watchstats.py "zp_${TAG}_$G" | grep -E 'glide|corner' | sed 's/^/  /'
bash $T/dfr.sh "smoothpan diag" | grep -E 'compositor=|zoom=|watch=' | cut -c1-170 | sed 's/^/  /'
