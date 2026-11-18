#!/bin/bash
# tools/wmmt4-longrun.sh <seconds> <logfile> <shotdir>
# Headless measured run of WMN4r under linuxloader: boot/shots tracing on,
# stops the game's own process group after the given time.
set -u
secs=${1:-240}
log=${2:-/tmp/wmmt4-long.log}
shots=${3:-/tmp/wmmt4-shots-long}
R=/mnt/MoreStuff/linuxloader-rawthrills
cd /mnt/Games2/wmmt4 || exit 1

# leftover instances break the boot: only ld-linux-hosted WMN4r processes
for p in $(pgrep -f WMN4r); do
    case "$(readlink /proc/$p/exe 2>/dev/null)" in
    *ld-linux*) kill -9 "$p" 2>/dev/null ;;
    esac
done
sleep 1

if ! DISPLAY=:99 xdpyinfo >/dev/null 2>&1; then
    setsid Xvfb :99 -screen 0 1280x800x24 -nolisten tcp >/dev/null 2>&1 &
    sleep 1
fi

[ -n "$shots" ] && { rm -rf "$shots"; mkdir -p "$shots"; }
# set -m: the background job gets its own process group (the pid), so the
# kill below stops the game and nothing else. No setsid: it would fork and
# make $! a process that is gone at once.
set -m
if [ -n "$shots" ]; then
    export WMMT4_SHOT="$shots" WMMT4_SHOT_PERIOD=5
fi
LD_LIBRARY_PATH="$PWD/lib:$R/libs/linux_x86:$R/build-linux" \
LD_PRELOAD="$R/build-linux/linuxloader.so" \
DISPLAY=:99 SDL_AUDIODRIVER=dummy MESA_LOADER_DRIVER_OVERRIDE=swrast \
NAMCO_GAME_LOG=1 WMMT4_BOOT_TRACE=1 WMMT4_AUTH_TRACE=1 \
/lib/ld-linux.so.2 ./WMN4r >"$log" 2>&1 &
pid=$!
echo "started pid $pid, log $log, shots ${shots:--}"
for i in $(seq "$secs"); do
    sleep 1
    kill -0 "$pid" 2>/dev/null || {
        echo "game exited by itself after ${i}s"
        break
    }
done
kill -9 -- -"$pid" 2>/dev/null
wait "$pid" 2>/dev/null
caps=$([ -n "$shots" ] && ls "$shots" | wc -l || echo 0)
echo "stopped; $(wc -l <"$log") log lines, $caps captures"
