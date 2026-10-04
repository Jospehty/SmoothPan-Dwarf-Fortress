#!/usr/bin/env bash
# Bottom-edge pixel column at x=1800 with the compositor on vs off, plus DF's
# viewport geometry.  Same view, no motion.
T="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
D="/home/jospeh/.local/share/Steam/steamapps/common/Dwarf Fortress/dfhack-config/smoothpan"
col() { magick "$D/shot_$1.ppm" -crop 1x24+1800+1416 +repage txt:- | awk 'NR>1{printf "%s ", $3}'; echo; }
bash $T/dfr.sh "smoothpan watch shot comp_on" >/dev/null; sleep 0.6
bash $T/dfr.sh "smoothpan compositor off" >/dev/null; sleep 0.6
bash $T/dfr.sh "smoothpan watch shot comp_off" >/dev/null; sleep 0.6
bash $T/dfr.sh "smoothpan compositor on" >/dev/null
echo "rows 1416..1439 @x=1800"
echo "ON : $(col comp_on)"
echo "OFF: $(col comp_off)"
bash $T/dflua.sh "local g=df.global.gps; local v=g.main_viewport; print('vp screen', v.screen_x, v.screen_y, 'dim', v.dim_x, v.dim_y, 'zoom', g.viewport_zoom_factor, 'screen_px', g.screen_pixel_x, g.screen_pixel_y)"
