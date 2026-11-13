#!/bin/bash
# Add glass sound to Joe's CapCut window transitions.
#   blowout: still glass -> shatter at 5.67 s -> black. Real smash (SFX track
#            26.0 s hit) lands on the first shatter frame; video holds black
#            0.55 s longer so the smash rings out instead of being cut off.
#   restore: black -> shards fly back -> whole at ~0.97 s. A smash played
#            backwards (SFX 13.4 s hit) swells and "snaps" shut on that frame.
# The CapCut audio is near-silent (-37 dB peak) and is replaced.
set -euo pipefail
BASE="/mnt/media/Stain Glass 2026/Main 2026/Transitions"
SRC="$BASE/CapCut source"
cd "$BASE"
SFX="/mnt/media/Remote Media/Projector/Halloween/Sound-Music/OLD CD's/175 Ultimate Halloween Sound Effects - 2CD/CD 2/84 - The Hit Crew - Breaking Glass.mp3"

# Static gain from each SFX slice's own peak (loudnorm is unreliable on <3 s clips).
gain() { # $1=start $2=dur $3=target peak dB
  local pk; pk=$(ffmpeg -hide_banner -ss "$1" -t "$2" -i "$SFX" -af volumedetect -f null - 2>&1 | sed -n 's/.*max_volume: \(-*[0-9.]*\) dB/\1/p')
  python3 -c "print(round($3 - ($pk), 2))"
}
G_BLOW=$(gain 26.0 2.0 -3)
G_REST=$(gain 13.4 0.85 -6)
dur() { ffprobe -v error -select_streams v:0 -show_entries stream=duration -of csv=p=0 "$1"; }  # video stream length
# apad + -shortest errors at EOF on this ffmpeg ("No space left on device"),
# so pad audio to an exact length instead.
D_BLOW=$(python3 -c "print($(dur "$SRC/Glass Blow out before action.avi") + 0.55)")
D_REST=$(dur "$SRC/after action glass coming back.avi")

ffmpeg -v error -y -i "$SRC/Glass Blow out before action.avi" -ss 26.0 -t 2.0 -i "$SFX" -filter_complex "
  [0:v]tpad=stop_mode=add:stop_duration=0.55:color=black,format=yuv420p[v];
  [1:a]aresample=48000,volume=${G_BLOW}dB,afade=t=out:st=1.0:d=0.4,adelay=5670|5670,apad=whole_dur=${D_BLOW},atrim=end=${D_BLOW}[a]" \
  -map "[v]" -map "[a]" -c:v libx264 -preset slow -crf 16 -r 30 \
  -c:a aac -b:a 192k -ar 48000 -movflags +faststart glass_blowout.mp4

# reversed smash rendered to a temp wav first
REV=$(mktemp --suffix=.wav)
ffmpeg -v error -y -ss 13.4 -t 0.85 -i "$SFX" -af "aresample=48000,areverse,volume=${G_REST}dB,afade=t=in:d=0.15,afade=t=out:st=0.82:d=0.03" "$REV"
ffmpeg -v error -y -i "$SRC/after action glass coming back.avi" -i "$REV" -filter_complex "
  [0:v]format=yuv420p[v];
  [1:a]adelay=120|120,apad=whole_dur=${D_REST},atrim=end=${D_REST}[a]" \
  -map "[v]" -map "[a]" -c:v libx264 -preset slow -crf 16 -r 30 \
  -c:a aac -b:a 192k -ar 48000 -movflags +faststart glass_restore.mp4
rm -f "$REV"

ls -la "$BASE"
