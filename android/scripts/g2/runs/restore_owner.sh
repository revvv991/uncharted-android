#!/usr/bin/env bash
# restore_owner.sh [stamp]: the owner's launcher files and saves back from a run's backup pair
# (cfg-backup-<stamp>/user-backup-<stamp>; the newest pair by default) after a run that could not
# put them back itself - adb dropped, the battery ran down, a script was killed mid-run. Then the
# launcher's settings as the owner has them (1280x720, the 30 FPS limit, the touch pad auto,
# Turnip) with their More environment taken from the backup's env.txt. Refuses while the game runs.
# Afterwards: `ls -d files/*backup*` shows the pairs; old ones may be deleted by hand.
set -uo pipefail
. "$(dirname -- "${BASH_SOURCE[0]}")/g2.sh"
if game_running; then echo "the game is running: not restoring"; exit 1; fi
stamp=${1:-$($ADB shell "ls -d $G/cfg-backup-* | sort | tail -1" | tr -d '\r' | sed 's/.*cfg-backup-//')}
[ -n "$stamp" ] || { echo "no backup pair on the device"; exit 1; }
$ADB shell "[ -d $G/cfg-backup-$stamp ] && [ -d $G/user-backup-$stamp ]" || { echo "no such pair: $stamp"; exit 1; }
echo "restoring from cfg-backup-$stamp and user-backup-$stamp"
oenv=$($ADB shell "grep -v -E '$LAUNCHER_KEYS' $G/cfg-backup-$stamp/env.txt" | tr -d '\r' | sed '/^$/d' | paste -sd'\n' -)
echo "owner's extra environment (from the backup): [${oenv}]"
put_back "$stamp" "$(printf '%s' "$oenv" | sed ':a;N;$!ba;s/\n/\\\\n/g')"
$ADB shell "cd $G && grep -v -E '$LAUNCHER_KEYS' env.txt | tr '\n' ' '; echo; grep -E 'fps_limit|upscaler=' bbport.ini | tr '\n' ' '; echo"
