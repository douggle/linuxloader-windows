#!/usr/bin/env bash
# Installs the WMMT4 service fronts on the douggle.us box.
#
#   mucha  : TLS 1.0 on :10082 -> mucha_backend.py  (127.0.0.1:11082)
#            /mucha_front/boardauth.do, /mucha_front/updatacheck.do
#   rpc    : TLS 1.0 on :9002  -> rpc_backend.py    (127.0.0.1:19002)
#            /method/<name> (protobuf), /resource/<name>
#
# Files:
#   <script>                        -> /opt/wmmt4/
#   <unit>.service                  -> /etc/systemd/system/
#   mucha.crt / mucha.key           -> /etc/nginx/tls/          (RSA, self-signed)
#   nginx/stream.d/*.conf           -> /etc/nginx/stream.d/
#   stream { include ... }          -> /etc/nginx/nginx.conf    (backed up, tested)
#
# 80/443 are not touched: both listeners are separate ports. nginx.conf is
# backed up before the edit and restored if `nginx -t` rejects it.
#
# Usage, as root, from the directory holding this script:
#   sudo bash install.sh
#   bash install.sh --check      # read-only: say what would change
#
# Knobs: PREFIX, CERT_DIR, CN, USE_STUNNEL=1 (skip nginx).
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
PREFIX=${PREFIX:-/opt/wmmt4}
CERT_DIR=${CERT_DIR:-/etc/nginx/tls}
CERT=$CERT_DIR/mucha.crt
KEY=$CERT_DIR/mucha.key
CN=${CN:-v388-front.mucha-prd.nbgi-amnet.jp}
STREAM_DIR=/etc/nginx/stream.d
NGINX_CONF=/etc/nginx/nginx.conf
SYSTEMD=/etc/systemd/system

# name  listen  backend  script               unit
FRONTS=(
    "mucha 10082 11082 mucha_backend.py mucha-backend"
    "rpc   9002  19002 rpc_backend.py   wmmt4-rpc-backend"
)

CHECK=0
[ "${1:-}" = "--check" ] && CHECK=1

say()   { printf '\n== %s\n' "$*"; }
warn()  { printf 'warning: %s\n' "$*" >&2; }
die()   { printf 'error: %s\n' "$*" >&2; exit 1; }
would() { if [ "$CHECK" = 1 ]; then printf '   would: %s\n' "$*"; else printf '   %s\n' "$*"; fi; }

if [ "$CHECK" = 0 ]; then
    [ "$(id -u)" -eq 0 ] || die "run as root: sudo bash $0   (or --check to inspect)"
fi
command -v python3 >/dev/null || die "python3 is required"

SKIP_NGINX=0
if [ "${USE_STUNNEL:-0}" = 1 ]; then
    SKIP_NGINX=1
    say "stunnel mode: use stunnel-10082.conf as a template for both ports"
else
    command -v nginx >/dev/null || die "nginx not found; run USE_STUNNEL=1 and see stunnel-10082.conf"
    nginx -V 2>&1 | tr ' ' '\n' | grep -qx -- '--with-stream' \
        || die "this nginx has no stream module; run USE_STUNNEL=1 and see stunnel-10082.conf"
fi

log_dir_for() {
    case "$1" in
        mucha) echo /var/log/mucha ;;
        rpc)   echo /var/log/wmmt4-rpc ;;
        *)     echo "/var/log/wmmt4-$1" ;;
    esac
}

# Free a listening port: nginx may keep it (it is reloaded), anything else is
# stopped, so a leftover socat test cannot keep the TLS listener from binding.
free_port() {
    local port=$1 line pid name
    line=$(ss -tlnp 2>/dev/null | grep ":$port " | tr -s ' ' || true)
    [ -n "$line" ] || return 0
    if printf '%s' "$line" | grep -q nginx; then
        would ":$port is nginx's; replaced on reload"
        return 0
    fi
    pid=$(printf '%s' "$line" | grep -o 'pid=[0-9]*' | head -1 | cut -d= -f2 || true)
    name=$(ps -o comm= -p "${pid:-0}" 2>/dev/null || echo unknown)
    [ -n "${pid:-}" ] || die ":$port is held by something ss cannot identify; find it with: ss -tlnp | grep $port"
    would "stop $name (pid $pid) holding :$port"
    if [ "$CHECK" = 0 ]; then
        kill "$pid" || die "could not stop pid $pid"
        sleep 1
        # `if` rather than `&&`: a freed port must not make this function
        # return non-zero, or `set -e` ends the install right here.
        if ss -tln | grep -q ":$port "; then
            die "$name (pid $pid) still holds :$port"
        fi
    fi
}

say "certificate"
would "reuse $CERT if present, else create an RSA self-signed one (CN=$CN)"
if [ "$CHECK" = 0 ]; then
    install -d -m 0755 "$CERT_DIR"
    if [ -s "$CERT" ] && [ -s "$KEY" ]; then
        would "keeping the existing certificate"
    else
        openssl req -x509 -newkey rsa:2048 -nodes -days 3650 -subj "/CN=$CN" \
            -keyout "$KEY" -out "$CERT" 2>/dev/null
        chmod 600 "$KEY"
    fi
fi

for front in "${FRONTS[@]}"; do
    read -r name port backend script unit <<< "$front"
    logdir=$(log_dir_for "$name")
    unitfile=$SYSTEMD/$unit.service

    say "$name front: :$port -> 127.0.0.1:$backend"
    free_port "$port"

    would "install $script -> $PREFIX/"
    if [ "$CHECK" = 0 ]; then
        install -d -m 0755 "$PREFIX"
        install -m 0755 "$HERE/$script" "$PREFIX/$script"
        install -d -m 0755 "$logdir"
        : >> "$logdir/$name.log"
    fi

    would "write $unitfile, enable and restart $unit"
    if [ "$CHECK" = 0 ]; then
        case "$name" in
            mucha) env_lines="Environment=MUCHA_LOG=$logdir/mucha.log
Environment=MUCHA_PORT=$backend" ;;
            rpc)   env_lines="Environment=RPC_LOG=$logdir/rpc.log
Environment=RPC_PORT=$backend
Environment=RPC_DUMP=$logdir/dump
Environment=RPC_USERS=$logdir/users.json" ;;
            *)     env_lines="" ;;
        esac
        cat > "$unitfile" <<UNIT
[Unit]
Description=WMMT4 $name backend (plaintext HTTP behind the TLS 1.0 front)
After=network.target

[Service]
ExecStart=/usr/bin/python3 $PREFIX/$script
$env_lines
Restart=always
RestartSec=2
NoNewPrivileges=yes
PrivateTmp=yes
ProtectSystem=strict
ReadWritePaths=$logdir

[Install]
WantedBy=multi-user.target
UNIT
        systemctl daemon-reload
        systemctl enable "$unit" >/dev/null
        systemctl restart "$unit"
    fi
done

if [ "$SKIP_NGINX" = 0 ]; then
    say "nginx stream listeners"
    would "install $STREAM_DIR/{mucha-10082,rpc-9002}.conf"
    if [ "$CHECK" = 0 ]; then
        install -d -m 0755 "$STREAM_DIR"
        for front in "${FRONTS[@]}"; do
            read -r name port backend script unit <<< "$front"
            install -m 0644 "$HERE/nginx/stream.d/$name-$port.conf" "$STREAM_DIR/$name-$port.conf"
        done
    fi

    if grep -qs 'stream\.d/\*\.conf' "$NGINX_CONF"; then
        would "$NGINX_CONF already includes stream.d; test and reload for the current fragments"
        if [ "$CHECK" = 0 ]; then
            if ! nginx -t >/tmp/nginx-test.out 2>&1; then
                cat /tmp/nginx-test.out >&2
                die "nginx -t failed; nothing was reloaded"
            fi
            systemctl reload nginx
            would "nginx reloaded"
        fi
    else
        backup=$NGINX_CONF.bak.$(date +%Y%m%d%H%M%S)
        would "back up $NGINX_CONF -> $backup, add the top-level stream block, nginx -t, reload"
        if [ "$CHECK" = 0 ]; then
            cp -a "$NGINX_CONF" "$backup"
            if grep -qE '^[[:space:]]*stream[[:space:]]*\{' "$NGINX_CONF"; then
                awk 'BEGIN { done = 0 }
                     { print }
                     !done && /^[[:space:]]*stream[[:space:]]*\{/ {
                         print "    include /etc/nginx/stream.d/*.conf;"; done = 1
                     }' "$NGINX_CONF" > "$NGINX_CONF.tmp"
                mv "$NGINX_CONF.tmp" "$NGINX_CONF"
            else
                printf '\nstream {\n    include /etc/nginx/stream.d/*.conf;\n}\n' >> "$NGINX_CONF"
            fi
            if ! nginx -t >/tmp/nginx-test.out 2>&1; then
                cp -a "$backup" "$NGINX_CONF"
                cat /tmp/nginx-test.out >&2
                die "nginx rejected the new config; $NGINX_CONF restored from $backup"
            fi
            systemctl reload nginx
            would "nginx reloaded"
        fi
    fi
fi

if [ "$CHECK" = 0 ]; then
    say "verifying"
    sleep 1
    ss -tlnp 2>/dev/null | grep -E ":10082 |:9002 |:11082 |:19002 " | tr -s ' ' || warn "nothing listening yet"
    printf 'gameCd=WM41&gameVer=WM410JPN10.18&placeId=JPN0JPN0' |
        curl -s --max-time 5 --data-binary @- "http://127.0.0.1:11082/mucha_front/boardauth.do" | cut -c1-120
    printf '\nregister_system_info (protobuf, hex): '
    printf '\x08\x01' | curl -s --max-time 5 --data-binary @- \
        "http://127.0.0.1:19002/method/register_system_info" | od -An -tx1 | tr -d '\n'
    printf '\n'
    for front in "${FRONTS[@]}"; do
        read -r name port backend script unit <<< "$front"
        printf '%s :%s  ' "$name" "$port"
        timeout 10 openssl s_client -connect "127.0.0.1:$port" -tls1 \
            -cipher 'AES128-SHA:@SECLEVEL=0' -brief </dev/null 2>&1 |
            grep -E 'CONNECTION ESTABLISHED|Protocol version|alert|error' | head -1
    done

    say "done"
    cat <<'NEXT'
From the cabinet machine, both must say CONNECTION ESTABLISHED / TLSv1:
  openssl s_client -connect 40.160.55.11:10082 -tls1 -cipher 'AES128-SHA:@SECLEVEL=0' -brief </dev/null
  openssl s_client -connect 40.160.55.11:9002  -tls1 -cipher 'AES128-SHA:@SECLEVEL=0' -brief </dev/null
Mucha calls: /var/log/mucha/mucha.log
RPC calls:   /var/log/wmmt4-rpc/rpc.log, bodies in /var/log/wmmt4-rpc/dump/*.pb
NEXT
else
    say "check only: nothing was changed"
fi
