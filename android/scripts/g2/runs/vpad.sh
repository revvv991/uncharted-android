#!/usr/bin/env bash
# vpad.sh: a virtual controller on the G2 through Android's `uinput` tool, for input tests the
# built-in pad cannot do (issue #16: a joystick SDL has no mapping for). The G2's firmware takes
# the first external joystick and shows Android a normalized gamepad in its place (2022:3001, the
# same name; the raw device never reaches Android), which SDL maps. Only the first: so start makes
# a decoy for it ("Test Decoy", X/Y and one button), then "Test DInput Pad", which Android gets
# as it is - a pad in a PC / DirectInput mode: sixteen generic buttons (BTN_TRIGGER..BTN_DEAD:
# Android's BUTTON_1..16, SDL's joystick buttons 20..35), two sticks (X/Y and Z/RZ, centred), two
# triggers (BRAKE and GAS, resting low; Android reports each twice, as LTRIGGER and RTRIGGER too)
# and a hat. Android makes it a joystick, SDL gives it no mapping.
#   vpad.sh start              both devices appear (they live until stop, or for 4 hours)
#   vpad.sh press <code> [s]   a button (288..303) held for s seconds (0.25)
#   vpad.sh push <code> <v>    an axis to v for 0.3 s and back to rest: 0 X, 1 Y, 2 Z, 5 RZ (0..255,
#                              rest 128); 9 GAS, 10 BRAKE (0..255, rest 0); 16 HAT0X, 17 HAT0Y (-1..1)
#   vpad.sh axis <code> <v>    an axis to v
#   vpad.sh stop               the devices go
set -uo pipefail
. "$(dirname -- "${BASH_SOURCE[0]}")/g2.sh"
PIPE=/data/local/tmp/vpad.pipe
ID=2
send() { $ADB shell "echo '$1' > $PIPE"; }
ev() { send "{\"id\":$ID,\"command\":\"inject\",\"events\":[$1]}"; }
rest() { case "$1" in 9|10|16|17) echo 0;; *) echo 128;; esac; }
info='"minimum":0,"maximum":255,"fuzz":0,"flat":0,"resolution":0'
hat='"minimum":-1,"maximum":1,"fuzz":0,"flat":0,"resolution":0'
case "${1:-}" in
start)
    decoy="{\"id\":1,\"command\":\"register\",\"name\":\"Test Decoy\",\"vid\":4660,\"pid\":22129,\"bus\":\"usb\",\"configuration\":[{\"type\":100,\"data\":[1,3]},{\"type\":101,\"data\":[304]},{\"type\":103,\"data\":[0,1]}],\"abs_info\":[{\"code\":0,\"info\":{\"value\":128,$info}},{\"code\":1,\"info\":{\"value\":128,$info}}]}"
    abs=""
    for code in 0 1 2 5; do abs="$abs{\"code\":$code,\"info\":{\"value\":128,$info}},"; done
    for code in 9 10; do abs="$abs{\"code\":$code,\"info\":{\"value\":0,$info}},"; done
    abs="$abs{\"code\":16,\"info\":{\"value\":0,$hat}},{\"code\":17,\"info\":{\"value\":0,$hat}}"
    pad="{\"id\":$ID,\"command\":\"register\",\"name\":\"Test DInput Pad\",\"vid\":4660,\"pid\":22141,\"bus\":\"usb\",\"configuration\":[{\"type\":100,\"data\":[1,3]},{\"type\":101,\"data\":[$(seq -s, 288 303)]},{\"type\":103,\"data\":[0,1,2,5,9,10,16,17]}],\"abs_info\":[$abs]}"
    $ADB shell "rm -f $PIPE && mkfifo $PIPE && (nohup sleep 14400 > $PIPE 2>/dev/null &) && (nohup uinput $PIPE > /data/local/tmp/vpad.log 2>&1 &) && sleep 0.5"
    send "$decoy"; sleep 1.5
    send "$pad"; sleep 1.5
    $ADB shell 'for e in /sys/class/input/event*; do echo "$(cat $e/device/id/vendor):$(cat $e/device/id/product) $(cat $e/device/name)"; done | grep Test'
    ;;
press) ev "1,$2,1,0,0,0"; sleep "${3:-0.25}"; ev "1,$2,0,0,0,0";;
push) ev "3,$2,$3,0,0,0"; sleep 0.3; ev "3,$2,$(rest "$2"),0,0,0";;
axis) ev "3,$2,$3,0,0,0";;
# The bracket keeps pkill -f from matching the adb shell's own command line (it killed it).
stop) $ADB shell "pkill -f 'commands[.]uinput'; pkill -f 'sleep 1440[0]'; rm -f $PIPE /data/local/tmp/vpad.log";;
*) echo "usage: vpad.sh start | press <code> [secs] | push <code> <value> | axis <code> <value> | stop"; exit 1;;
esac
