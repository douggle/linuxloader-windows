#!/usr/bin/env bash
# Deploy the Windows loader to cabinet 2 (192.168.1.189) over SMB.
#
#   scripts/linux/deploy-win32-cab2.sh [-f|--force] [LOCAL_EXE]
#
# Copies the Windows build to \\192.168.1.189\linuxloader-rawthrills\build-win32,
# keeps whatever was there as linuxloader.exe.bak.<timestamp>, then reads the
# file back and compares its SHA-256, so a half-written transfer cannot pass as
# a deploy. Credentials come from ~/.smbcred-cab2 through smbclient's -A, so the
# password is never on a command line. With --force it copies even when the cab
# already has the same file.
#
# The credentials file is smbclient's own format, readable by you only:
#
#   username=THE_USER
#   password=THE_PASSWORD
#   domain=WORKGROUP
#
#   chmod 600 ~/.smbcred-cab2
#
# Environment overrides: CAB2_HOST, CAB2_SHARE, CAB2_DIR, CAB2_NAME, SMB_CREDS,
# SMB_TIMEOUT (seconds, default 300).
set -o pipefail

host=${CAB2_HOST:-192.168.1.189}
share=${CAB2_SHARE:-linuxloader-rawthrills}
remote_dir=${CAB2_DIR:-build-win32}
remote_name=${CAB2_NAME:-linuxloader.exe}
creds=${SMB_CREDS:-$HOME/.smbcred-cab2}

usage() {
    sed -n '3,5p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
}

force=0
local_exe=
for arg in "$@"; do
    case $arg in
        -f | --force) force=1 ;;
        -h | --help)
            usage
            exit 0
            ;;
        -*)
            echo "Unknown option: $arg" >&2
            exit 2
            ;;
        *) local_exe=$arg ;;
    esac
done

repo_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
local_exe=${local_exe:-$repo_dir/$remote_dir/$remote_name}
tmp=$(mktemp -d)
trap 'rm -rf -- "$tmp"' EXIT

die() {
    echo "$*" >&2
    exit 1
}
hash_of() { sha256sum -- "$1" | cut -d' ' -f1; }

command -v smbclient >/dev/null || die "smbclient is not installed"
command -v sha256sum >/dev/null || die "sha256sum is not installed"
[ -f "$local_exe" ] || die "No such file: $local_exe (build it with: make win32)"
[ -f "$creds" ] || die "No credentials file: $creds (format is in the comments at the top of this script)"

# The file holds a password; warn rather than refuse, in case of an odd umask.
mode=$(stat -c %a "$creds" 2>/dev/null || echo '')
[ "$mode" = 600 ] || echo "warning: $creds is mode ${mode:-unknown}, not 600" >&2

smb() { timeout "${SMB_TIMEOUT:-300}" smbclient "//$host/$share" -A "$creds" -c "$1"; }

local_hash=$(hash_of "$local_exe")
echo "cabinet 2: //$host/$share/$remote_dir/$remote_name"

# The cab may have nothing there yet; that is not an error.
current_hash=
if smb "cd $remote_dir; get $remote_name $tmp/current" >/dev/null 2>&1; then
    current_hash=$(hash_of "$tmp/current")
    echo "  there: $(stat -c %s "$tmp/current") bytes, sha256 $current_hash"
else
    echo "  there: nothing"
fi
echo "  here:  $(stat -c %s "$local_exe") bytes, sha256 $local_hash"

if [ "$current_hash" = "$local_hash" ] && [ "$force" -eq 0 ]; then
    echo "Already up to date; nothing to do (--force copies anyway)."
    exit 0
fi

backup=
if [ -n "$current_hash" ]; then
    backup="$remote_name.bak.$(date +%Y%m%d-%H%M%S)"
    echo "Backing the old file up as $backup"
    smb "cd $remote_dir; rename $remote_name $backup" >/dev/null ||
        die "Could not move the existing $remote_name aside (is the loader running on the cab?)"
fi

echo "Uploading $(basename -- "$local_exe")"
smb "cd $remote_dir; put \"$local_exe\" $remote_name" >/dev/null || die "Upload failed"

echo "Reading it back"
smb "cd $remote_dir; get $remote_name $tmp/readback" >/dev/null || die "Could not read the file back"
readback_hash=$(hash_of "$tmp/readback")
if [ "$readback_hash" != "$local_hash" ]; then
    echo "Hash mismatch: the cab has $readback_hash, expected $local_hash" >&2
    if [ -n "$backup" ]; then
        echo "Putting $backup back" >&2
        smb "cd $remote_dir; del $remote_name; rename $backup $remote_name" >/dev/null ||
            echo "warning: restore failed; $backup is still on the cab" >&2
    fi
    exit 1
fi

echo "Deployed: the cab has $readback_hash, the same as $local_exe"
[ -n "$backup" ] && echo "Previous file kept as $backup"
exit 0
