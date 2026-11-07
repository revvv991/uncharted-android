#!/usr/bin/env bash
# np3_frames.sh <video> <out.png> [start s] [frames] [step] [crop w:h:x:y] [scale]: a contact sheet
# of <frames> frames (every <step>th) from <start>, cropped, 4 columns, scaled by <scale>.
# Runs in WSL (the ffmpeg of ~/venv-ff).
set -e
FF=$(ls ~/venv-ff/lib/python3.12/site-packages/imageio_ffmpeg/binaries/ffmpeg-linux-x86_64-* | head -1)
in=$1 out=$2 start=${3:-2} n=${4:-12} step=${5:-1} crop=${6:-1000:800:900:300} scale=${7:-0.5}
rows=$(( (n + 3) / 4 ))
"$FF" -v error -y -ss "$start" -i "$in" \
    -vf "select='not(mod(n\,$step))',crop=$crop,scale=iw*$scale:ih*$scale,tile=4x$rows:padding=4:color=white" \
    -frames:v 1 -fps_mode passthrough "$out"
echo "$out"
