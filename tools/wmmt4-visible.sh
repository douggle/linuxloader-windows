#!/bin/bash
# tools/wmmt4-visible.sh <seconds> <logfile>
#
# The same run as tools/wmmt4-longrun.sh, but on the desktop's own display
# (DISPLAY, default :0) instead of Xvfb :99, so the cabinet screen is visible
# and you can see when it reaches the card prompt. No frame captures.
#
# Server forwarding is whatever the caller sets:
#   WMMT4_SERVER_HOST=40.160.55.11 WMMT4_SERVER_PORTS=9002,10082 \
#       bash tools/wmmt4-visible.sh 1800 /tmp/card.log
set -u
secs=${1:-1800}
log=${2:-/tmp/wmmt4-visible.log}
R=/mnt/MoreStuff/linuxloader-rawthrills
cd /mnt/Games2/wmmt4 || exit 1
: "${DISPLAY:=:0}"

# leftover instances break the boot: only ld-linux-hosted WMN4r processes
for p in $(pgrep -f WMN4r); do
    case "$(readlink /proc/$p/exe 2>/dev/null)" in
    *ld-linux*) kill -9 "$p" 2>/dev/null ;;
    esac
done
sleep 1

# set -m: the background job gets its own process group (the pid), so the kill
# below stops the game and nothing else.
set -m
LD_LIBRARY_PATH="$PWD/lib:$R/libs/linux_x86:$R/build-linux" \
LD_PRELOAD="$R/build-linux/linuxloader.so" \
DISPLAY="$DISPLAY" SDL_AUDIODRIVER="${SDL_AUDIODRIVER:-dummy}" MESA_LOADER_DRIVER_OVERRIDE=swrast \
NAMCO_GAME_LOG=1 WMMT4_BOOT_TRACE=1 WMMT4_AUTH_TRACE=1 \
/lib/ld-linux.so.2 ./WMN4r >"$log" 2>&1 &
pid=$!
echo "started pid $pid on DISPLAY=$DISPLAY, log $log, ${secs}s"
if [ "$secs" -gt 0 ]; then
    for i in $(seq "$secs"); do
        sleep 1
        kill -0 "$pid" 2>/dev/null || {
            echo "game exited by itself after ${i}s"
            break
        }
    done
else
    # 0 seconds: run until the cabinet exits on its own (the app-menu launcher).
    wait "$pid" 2>/dev/null
    echo "game exited"
fi
kill -9 -- -"$pid" 2>/dev/null
wait "$pid" 2>/dev/null
echo "stopped; $(wc -l <"$log") log lines"
