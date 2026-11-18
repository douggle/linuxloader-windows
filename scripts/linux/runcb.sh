#!/usr/bin/env bash
# Cruis'n Blast (Raw Thrills): cabinet 1 of the linked pair.
#
# Runs the loader with this cabinet's config and keeps everything it prints, so
# the link line can be read back afterwards:
#
#   Raw Thrills: link address 192.168.1.127 (machine ID 1)
#
# GAME_DIR  the game folder (else $GAMES_DIR/CruisnBlast)
# CONFIG    the ini (default configs/cruisn-blast-cab1/linuxloader-linux.ini)
# LOG       the log file (default /tmp/cruisn-blast-cab1.log)
#
# The cabinet at 192.168.1.189 runs the same game with the cab2 config and
# machine ID 2 in its Network Adjustments; this one wants machine ID 1.
set -o pipefail

here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo_dir=$(cd -- "$here/../.." 2>/dev/null && pwd)
# Staged next to a loader build (the cabinet's copy), the build beside the
# script is the one used; in the repo it is build-linux/.
loader_dir=${LOADER_DIR:-}
if [ -z "$loader_dir" ] && [ -f "$here/linuxloader" ]; then loader_dir=$here; fi
if [ -z "$loader_dir" ] && [ -f "$repo_dir/build-linux/linuxloader" ]; then loader_dir=$repo_dir/build-linux; fi
: "${loader_dir:?Set LOADER_DIR to the loader build (the directory with linuxloader)}"

game_dir=${GAME_DIR:-"${GAMES_DIR:?Set GAME_DIR or GAMES_DIR}/CruisnBlast"}
config=${CONFIG:-}
if [ -z "$config" ] && [ -f "$here/linuxloader.ini" ]; then config=$here/linuxloader.ini; fi
if [ -z "$config" ] && [ -n "$repo_dir" ]; then config=$repo_dir/configs/cruisn-blast-cab1/linuxloader-linux.ini; fi
log=${LOG:-/tmp/cruisn-blast-cab1.log}

[ -d "$game_dir" ] || { echo "No game folder: $game_dir" >&2; exit 1; }
[ -f "$config" ] || { echo "No config: $config" >&2; exit 1; }

cd -- "$game_dir" || exit 1
export LD_LIBRARY_PATH=$loader_dir:$repo_dir/libs/linux_x86${LIBS32:+:$LIBS32}${TP_LIBS:+:$TP_LIBS}
echo "Cruis'n Blast, cabinet 1: config $config"
echo "loader: $loader_dir/linuxloader"
echo "log: $log"

set +e
"$loader_dir/linuxloader" -g "$game_dir" -L "$loader_dir" -c "$config" "$@" 2>&1 | tee "$log"
status=${PIPESTATUS[0]}

echo
echo "link line:"
grep -E 'link address|link play' "$log" || echo "  (none - check [Network] IP_CAB1 and ENABLE_NETWORK_PATCHES)"
exit "$status"
