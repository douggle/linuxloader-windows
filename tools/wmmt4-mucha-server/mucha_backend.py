#!/usr/bin/env python3
"""Minimal Mucha front for a WMMT4 cabinet (WMN4r, 1.10.18), plaintext HTTP.

The cabinet talks HTTPS to v388-front.mucha-prd.nbgi-amnet.jp:10082 with an
OpenSSL 0.9.8a client, so it offers TLS 1.0 with 2010-era ciphers. Terminate
that in nginx (stream) or stunnel on 10082 and point it at this process; this
file only needs to be reachable from the terminator, e.g. 127.0.0.1:11082.

Answers the two calls the cabinet makes:

  POST /mucha_front/boardauth.do     shop, areas, the service URLs
  POST /mucha_front/updatacheck.do   the version the cabinet already has

Both bodies are the same shape the loader's in-process server answers
(src/loader/namco/namcoEs1Wmmt4Server.c), so a card session behaves as it does
locally. Each request is logged with its form, which is the interesting part:
the form carries gameCd, gameVer, serialNum, mac, boardId, placeId and more.

Run:  python3 mucha_backend.py          (MUCHA_BIND/MUCHA_PORT/MUCHA_LOG/env)
"""

import os
import sys
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlsplit

BIND = os.environ.get("MUCHA_BIND", "127.0.0.1")
PORT = int(os.environ.get("MUCHA_PORT", "11082"))
MUCHA_URL = os.environ.get("MUCHA_URL", "https://v388-front.mucha-prd.nbgi-amnet.jp:10082")
PLACE_ID = os.environ.get("MUCHA_PLACE_ID", "JPN0JPN0")
SHOP = os.environ.get("MUCHA_SHOP_NAME", "Raw Thrills")
LOG = os.environ.get("MUCHA_LOG", "")


def log(line):
    text = time.strftime("%Y-%m-%d %H:%M:%S ") + line
    sys.stderr.write(text + "\n")
    sys.stderr.flush()
    if LOG:
        try:
            with open(LOG, "a") as handle:
                handle.write(text + "\n")
        except OSError as error:
            sys.stderr.write(f"cannot write {LOG}: {error}\n")


def board_auth(form):
    place = (form.get("placeId") or [PLACE_ID])[0] or PLACE_ID
    local, utc = time.localtime(), time.gmtime()
    return (
        "RESULTS=001&AREA_0=008&AREA_0_EN=&AREA_1=009&AREA_1_EN=&AREA_2=010&AREA_2_EN="
        "&AREA_3=011&AREA_3_EN=&AREA_FULL_0=&AREA_FULL_0_EN=&AREA_FULL_1=&AREA_FULL_1_EN="
        "&AREA_FULL_2=&AREA_FULL_2_EN=&AREA_FULL_3=&AREA_FULL_3_EN=&AUTH_INTERVAL=86400"
        f"&CHARGE_URL={MUCHA_URL}/charge/&CONSUME_TOKEN=0&COUNTRY_CD=JPN"
        "&DONGLE_FLG=1&EXPIRATION_DATE=null"
        f"&FILE_URL={MUCHA_URL}/file/&FORCE_BOOT=0&PLACE_ID={place}"
        "&PREFECTURE_ID=14"
        f"&SERVER_TIME={time.strftime('%Y%m%d%H%M', local)}"
        f"&UTC_SERVER_TIME={time.strftime('%Y%m%d%H%M', utc)}"
        f"&SHOP_NAME={SHOP}&SHOP_NAME_EN={SHOP}&SHOP_NICKNAME={SHOP}&SHOP_NICKNAME_EN={SHOP}"
        f"&URL_1={MUCHA_URL}/url1/&URL_2={MUCHA_URL}/url2/&URL_3={MUCHA_URL}/url3/&USE_TOKEN=0"
    )


def update_check(form):
    version = (form.get("gameVer") or ["WM410JPN10.18"])[0] or "WM410JPN10.18"
    return (
        f"RESULTS=001&UPDATE_VER_1={version}&UPDATE_URL_1={MUCHA_URL}/updUrl1/"
        "&UPDATE_SIZE_1=9318000&UPDATE_CRC_1=55C4000000000000"
        f"&CHECK_URL_1={MUCHA_URL}/checkUrl/&EXE_VER_1={version}"
        "&INFO_SIZE_1=180&COM_SIZE_1=16384&COM_TIME_1=100&LAN_INFO_SIZE_1=180"
    )


class Handler(BaseHTTPRequestHandler):
    # The cabinet's client is HTTP/1.0-era: answer in 1.0 and close, no
    # chunked encoding, no keep-alive.
    protocol_version = "HTTP/1.0"

    def do_POST(self):
        try:
            length = int(self.headers.get("Content-Length") or 0)
        except ValueError:
            length = 0
        body = self.rfile.read(length) if length > 0 else b""
        self.answer(body)

    def do_GET(self):
        self.answer(b"")

    def answer(self, body):
        split = urlsplit(self.path)
        path = split.path
        query = split.query
        raw = body.decode("latin-1", "replace") or query
        form = parse_qs(raw, keep_blank_values=True)
        log(f"{self.client_address[0]} {self.command} {path} {raw}")

        if path.endswith("/boardauth.do"):
            status, text = 200, board_auth(form)
        elif path.endswith("/updatacheck.do"):
            status, text = 200, update_check(form)
        else:
            # Anything else gets the same "no work" answer the loader gives,
            # so a card session is not broken by a stray URL.
            status, text = (200 if path == "/" else 404), "RESULTS=001"

        payload = text.encode()
        self.send_response(status)
        self.send_header("Content-Type", "text/plain")
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)

    def log_message(self, fmt, *args):
        pass  # answer() logs; the default line is noise


if __name__ == "__main__":
    log(f"listening on {BIND}:{PORT}, speaking for {MUCHA_URL}")
    try:
        ThreadingHTTPServer((BIND, PORT), Handler).serve_forever()
    except KeyboardInterrupt:
        pass
