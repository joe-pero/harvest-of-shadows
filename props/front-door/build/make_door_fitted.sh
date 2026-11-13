#!/usr/bin/env bash
# Front Door: warp the door clips onto the door's white cloth, black everywhere else.
# Corners are street-view projector px (1080x1920). LIVE FIT 2026-10-27 (defaults below): machine-read from ArUco
# marker photos IMG_0313/0314 (make_door_markers.py + read_door_markers.py, which prints the env line), confirmed
# on the door with make_door_verify.py (IMG_0316/0317). ASPECT = the cloth's TRUE width/height from the photo
# (EXIF focal length), so the clip is cropped to the real door shape and nothing is stretched.
# History: 10-05 rectangle (505,485)-(1080,1515) from the human grid, then a BL_WIDEN=30 trapezoid guess - both
# void after Joe moved the projector 10-27. Re-fit after any projector move: docs/front-door.md (2026-10-27 RESULT).
# Pipeline: stored portrait (90 CCW) -> transpose=1 upright -> centre-crop to ASPECT -> stretch to the frame ->
# perspective (frame corners -> door corners) -> black outside the door (mask) -> transpose=2 back.
# Input "SD Card 2026/" (full-frame show set), output "SD Card 2026 - fitted/" (+ the show autorun.brs); IN=/OUT= override.
set -euo pipefail
IN=${IN:-"/mnt/media/Front Door 2026/SD Card 2026"}
OUT=${OUT:-"/mnt/media/Front Door 2026/SD Card 2026 - fitted"}
# Four independent corners (env overrides), defaults = the live 2026-10-27 fit.
TLX=${TLX:-487}; TLY=${TLY:-400}; TRX=${TRX:-1013}; TRY=${TRY:-438}
BRX=${BRX:-1034}; BRY=${BRY:-1417}; BLX=${BLX:-527}; BLY=${BLY:-1478}
ASPECT=${ASPECT:-0.433}   # 0 = use the corners' projector-px aspect
MASK="/mnt/media/Front Door 2026/Calibration/door_mask.png"
mkdir -p "$OUT"
[ -f "$IN/autorun.brs" ] && cp "$IN/autorun.brs" "$OUT/autorun.brs"
python3 - "$MASK" $TLX $TLY $TRX $TRY $BRX $BRY $BLX $BLY <<'PY'
import sys, numpy as np, cv2
m, *c = sys.argv[1], *map(int, sys.argv[2:])
a = np.zeros((1920, 1080, 4), np.uint8); a[..., 3] = 255
cv2.fillPoly(a, [np.array(c, np.int32).reshape(4, 2)], (0, 0, 0, 0), cv2.LINE_AA)
cv2.imwrite(m, a)
PY
# door aspect (mean width / mean height) -> centre-crop the upright 1080x1920 clip to it (height if the door is
# wider than 9:16, width if narrower), so nothing is stretched
CROP=$(python3 -c "
w=(($TRX-$TLX)+($BRX-$BLX))/2; h=(($BLY-$TLY)+($BRY-$TRY))/2
a=float('${ASPECT:-0}')        # true cloth width/height (read_door_markers.py); 0 = use projector px
if a: w, h = a, 1.0
cw, ch = (1080, int(round(1080*h/w/2))*2) if h/w <= 1920/1080 else (int(round(1920*w/h/2))*2, 1920)
print('%d:%d' % (cw, ch))")
echo "corners TL $TLX,$TLY TR $TRX,$TRY BR $BRX,$BRY BL $BLX,$BLY aspect ${ASPECT:-proj-px} crop $CROP"
for g in ${@:-Door_Loop.mp4 Childern_action_door.mp4 ROTLD_action_door.mp4}; do
  ffmpeg -v error -y -i "$IN/$g" -loop 1 -i "$MASK" -filter_complex \
    "[0:v]transpose=1,crop=${CROP},scale=1080:1920:flags=lanczos,perspective=x0=${TLX}:y0=${TLY}:x1=${TRX}:y1=${TRY}:x2=${BLX}:y2=${BLY}:x3=${BRX}:y3=${BRY}:sense=destination[w];[w][1:v]overlay=0:0:format=auto,transpose=2,format=yuv420p[o]" \
    -map "[o]" -map "0:a?" -shortest \
    -c:v libx264 -preset medium -crf 19 -profile:v high -level 4.2 \
    -c:a aac -b:a 192k -ar 48000 -movflags +faststart "$OUT/$g"
  echo "built $g"
done
