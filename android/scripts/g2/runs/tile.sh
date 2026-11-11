#!/usr/bin/env bash
# tile.sh: sourced by stick_probe.sh and touch_probe.sh. tile <mp4> <png> [fps] lays a recording's
# frames (fps a second, 2 by default) out 4 by 4 at 480 pixels wide, with the ffmpeg of the WSL
# venv ~/venv-ff (imageio_ffmpeg's binary; WSL paths are made with wslpath).
tile() {
    local mp4=$1 png=$2 fps=${3:-2} ff
    ff=$(wsl -e bash -lc '~/venv-ff/bin/python -c "import imageio_ffmpeg; print(imageio_ffmpeg.get_ffmpeg_exe())"' | tr -d '\r')
    MSYS_NO_PATHCONV=1 wsl -e "$ff" -loglevel error -y -i "$(wsl -e wslpath -a "$(cygpath -w "$mp4")")" \
        -vf "fps=$fps,scale=480:-1,tile=4x4" -frames:v 1 "$(wsl -e wslpath -a "$(cygpath -w "$png")")"
}
