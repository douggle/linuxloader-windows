# Mucha front for WMMT4 on douggle.us (40.160.55.11)

The cabinet (WMN4r 1.10.18) talks HTTPS to
`v388-front.mucha-prd.nbgi-amnet.jp:10082` with an **OpenSSL 0.9.8a** client:
TLS 1.0, RSA key exchange, `AES128-SHA`. A modern listener answers
`alert 70 (protocol version)` and the loader reports
`TLS handshake failed with 40.160.55.11:10082`.

This directory is everything needed to make 10082 answer that client:

| file | what it is |
|---|---|
| `mucha_backend.py` | plaintext HTTP backend, `127.0.0.1:11082`, answers `/mucha_front/boardauth.do` and `/mucha_front/updatacheck.do` |
| `rpc_backend.py` | plaintext protobuf backend, `127.0.0.1:19002`, answers `/method/register_system_info` fully and logs every other call |
| `nginx/stream.d/mucha-10082.conf` | nginx `stream` server: TLS 1.0 on `:10082` → `11082` |
| `nginx/stream.d/rpc-9002.conf` | nginx `stream` server: TLS 1.0 on `:9002` → `19002` |
| `install.sh` | puts all of it in place (`--check` for a dry run) |
| `stunnel-10082.conf` | stunnel pattern, if nginx has no stream module |

## Install

```sh
# 1. backend
sudo install -d /opt/wmmt4
sudo cp mucha_backend.py /opt/wmmt4/
sudo install -d /var/log/mucha
sudo tee /etc/systemd/system/mucha-backend.service >/dev/null <<'UNIT'
[Unit]
Description=WMMT4 Mucha backend
After=network.target
[Service]
ExecStart=/usr/bin/python3 /opt/wmmt4/mucha_backend.py
Environment=MUCHA_LOG=/var/log/mucha/mucha.log
Restart=always
[Install]
WantedBy=multi-user.target
UNIT
sudo systemctl enable --now mucha-backend

# 2. TLS 1.0 terminator - pick ONE
#    nginx: paste nginx-stream-10082.conf at the top level of
#           /etc/nginx/nginx.conf (outside the http block), then:
sudo nginx -t && sudo systemctl reload nginx
#    or stunnel: see the header of stunnel-10082.conf

# 3. certificate (RSA, self-signed is fine)
sudo mkdir -p /etc/nginx/tls
sudo openssl req -x509 -newkey rsa:2048 -nodes -days 3650 \
     -subj "/CN=v388-front.mucha-prd.nbgi-amnet.jp" \
     -keyout /etc/nginx/tls/mucha.key -out /etc/nginx/tls/mucha.crt
```

## Verify, in this order

```sh
# on the box: TLS 1.0 is accepted (must print CONNECTION ESTABLISHED, TLSv1)
openssl s_client -connect 127.0.0.1:10082 -tls1 \
        -cipher 'AES128-SHA:@SECLEVEL=0' -brief </dev/null

# on the box: the backend answers
printf 'gameCd=WM41&gameVer=WM410JPN10.18&placeId=JPN0JPN0' |
  curl -s --max-time 5 --data-binary @- http://127.0.0.1:11082/mucha_front/boardauth.do

# from the cabinet machine
openssl s_client -connect 40.160.55.11:10082 -tls1 \
        -cipher 'AES128-SHA:@SECLEVEL=0' -brief </dev/null
```

Then the cabinet run, from the cabinet:

```sh
WMMT4_SERVER_HOST=40.160.55.11 WMMT4_SERVER_PORTS=9002,10082 \
  bash tools/wmmt4-longrun.sh 90 /tmp/mucha-remote.log ""
```

What to look for: no `NOT forwarded` line for `boardauth.do`, and the backend
log (`/var/log/mucha/mucha.log`) showing the cabinet's form with
`serialNum=…&mac=BCFCE7E8A587&boardId=D1`.

## State as deployed (2026-11-18)

Both fronts terminate TLS 1.0 in nginx and proxy to a Python backend:

- `:10082` Mucha — verified end to end: the cabinet's own `boardauth.do` and
  `updatacheck.do` are answered by the VPS, and the cabinet logs
  `mucha: auth success`.
- `:9002` game server — `RegisterSystemInfo` completes against the VPS:
  `SSL handshake succeeded` → `send body, size=1214` →
  `service: ERR_SUCCESS` → `register_system_info: region_id=13, place_id=JPN0FFF0`.

Every other RPC (`load_user`, `create_car`, `ping`, …) is answered with error 2
(`ERR_NOT_FOUND`) on purpose, and the call is logged with its body in
`/var/log/wmmt4-rpc/dump/<method>.pb` (`protoc --decode_raw` reads it). Those
are the next pieces: a card session is what exercises them.
