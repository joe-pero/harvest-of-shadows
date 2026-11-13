#!/bin/bash
# Join the SD-card door clips into one loop video.
# In:  /mnt/media/Front Door 2026/Main 2026/sd card/
# Out: /mnt/media/Front Door 2026/SD Card 2026/Door_Loop.mp4
# Order: NS door group (buffer between NS clips only), Twins between door
# groups, TrT door group, Twins, No candy. Everything is normalised to
# 1920x1080 29.97 fps yuv420p + 48 kHz stereo AAC so the joins are clean.
set -euo pipefail
BASE="/mnt/media/Front Door 2026"
cd "$BASE/Main 2026/sd card"

clips=(
  NS_AnybodyHome_Door_V.mp4
  NS_Buffer_Door_V.mp4
  NS_PanicAttack_Door_V.mp4
  NS_Buffer_Door_V.mp4
  NS_RunAndHide_Door_V.mp4
  MM_Twins_DoubleTrouble_Win_V.mp4
  TrT_SeasonsGreetings_Door_V_Front.mp4
  TrT_SomethingGoodToEat_Door_V_Front.mp4
  TrT_StartleScares_Door_V_Front.mp4
  MM_Twins_KidsGames_Win_V.mp4
  "No candy.mp4"
)

inputs=() filt="" n=${#clips[@]}
for i in "${!clips[@]}"; do
  inputs+=(-i "${clips[$i]}")
  filt+="[$i:v]scale=1920:1080,setsar=1,fps=30000/1001,format=yuv420p[v$i];"
  filt+="[$i:a]aresample=48000,aformat=channel_layouts=stereo[a$i];"
done
for i in $(seq 0 $((n - 1))); do filt+="[v$i][a$i]"; done
filt+="concat=n=$n:v=1:a=1[v][a]"

ffmpeg -v error -stats -y "${inputs[@]}" -filter_complex "$filt" -map "[v]" -map "[a]" \
  -c:v libx264 -preset slow -crf 18 -c:a aac -b:a 192k -movflags +faststart \
  "$BASE/SD Card 2026/Door_Loop.mp4"
