#!/bin/sh
# nested-X smoke test for bawm + bawm-panel
DISP=:1
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/tmp/bawm-runtime-$$}"
mkdir -p "$XDG_RUNTIME_DIR"
DIR=$(cd "$(dirname "$0")" && pwd)

if [ -e /tmp/.X11-unix/X${DISP#:} ]; then
    echo "display $DISP already in use — killing old Xephyr"
    pkill -f "Xephyr $DISP" 2>/dev/null
    sleep 1
fi

Xephyr $DISP -screen 1280x800 -ac -noreset -title "bawm test" &
XEPHYR_PID=$!

i=0
while [ ! -e /tmp/.X11-unix/X${DISP#:} ]; do
    i=$((i+1))
    [ $i -gt 50 ] && { echo "Xephyr never came up"; kill $XEPHYR_PID; exit 1; }
    sleep 0.1
done
sleep 0.3

DISPLAY=$DISP "$DIR/bawm-panel" &
sleep 0.2
DISPLAY=$DISP "$DIR/bawm" 2>/tmp/bawm.log &
sleep 0.5

if [ -x "$HOME/.config/bawm/status.sh" ]; then
    DISPLAY=$DISP "$HOME/.config/bawm/status.sh" &
fi

for n in one two three four; do
    DISPLAY=$DISP xterm -fa monospace -fs 10 -title "$n" &
    sleep 0.3
done

echo
echo "bawm on $DISP  (pid $XEPHYR_PID)"
echo "Alt+1..9 workspaces / Alt+Shift+1..9 send / Alt+Shift+q quit"
echo "log: tail -f /tmp/bawm.log"
echo "kill: pkill -f 'Xephyr $DISP'"
wait $XEPHYR_PID
