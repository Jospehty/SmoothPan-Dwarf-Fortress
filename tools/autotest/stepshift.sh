#!/usr/bin/env bash
# Content displacement for one camera step, plugin inert (shift mode none,
# compositor off): a normal step (60->59) and a step across a screen_x wrap (44->43).
T="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
bash $T/dfr.sh "smoothpan compositor off" "smoothpan mode none" >/dev/null
shot() {
    bash $T/dflua.sh "df.global.window_x=$1" >/dev/null; sleep 0.4
    bash $T/dfr.sh "smoothpan watch shot $2" >/dev/null; sleep 0.4
    bash $T/dflua.sh "print('$2 wx', df.global.window_x, 'screen_x', df.global.gps.main_viewport.screen_x)"
}
shot 60 s60; shot 59 s59; shot 44 s44; shot 43 s43
bash $T/dfr.sh "smoothpan mode sdl" "smoothpan compositor on" >/dev/null
python3 $T/offset2.py s60 s59
python3 $T/offset2.py s44 s43
bash $T/recenter.sh
