#!/usr/bin/env python3
"""Minimal game-server (9002) backend for a WMMT4 cabinet, plaintext HTTP.

The cabinet's client speaks HTTPS to https://<uri>:9002/method/<name> (POST,
protobuf) and /resource/<name> (GET). Terminate TLS 1.0 in nginx (stream) or
stunnel on 9002 and point it here, e.g. 127.0.0.1:19002.

Answered here, matching src/loader/namco/namcoEs1Wmmt4Rpc.c:

  register_system_info  full 13-field reply (region 13, place JPN0FFF0, dates)
  ping                  error 0, value 1
  load_user             success for any card; an unseen card gets a new user id
                        (kept in RPC_USERS). Cars stored for that user are
                        returned in the repeated car fields.
  create_car            stores the car the game sends (model, colour, tuning)
                        with its name forced to RPC_CAR_NAME ("dougl": the
                        cabinet's name field is short), and answers the new
                        car's id and search code
  load_car              the stored car back, with its setting
  /resource/*           200 with an empty message
  anything else         error 2 (not found), logging the call so the next gap
                        in the chain is visible

Every request is logged to $RPC_LOG (default stderr) and its body kept in
$RPC_DUMP/<method>.pb, which `protoc --decode_raw < file` reads directly.

Run:  python3 rpc_backend.py
      (RPC_BIND/RPC_PORT/RPC_LOG/RPC_DUMP/RPC_USERS/RPC_CAR_NAME/env)
"""

import json
import os
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlsplit

BIND = os.environ.get("RPC_BIND", "127.0.0.1")
PORT = int(os.environ.get("RPC_PORT", "19002"))
LOG = os.environ.get("RPC_LOG", "")
DUMP = os.environ.get("RPC_DUMP", "")
USERS = os.environ.get("RPC_USERS", "")
CAR_NAME = os.environ.get("RPC_CAR_NAME", "dougl")
CONTENT_TYPE = "application/x-protobuf; revision=5112"

REGION_ID = 13          # Tokyo, as the loader answers
PLACE_ID = "JPN0FFF0"
FAR_FUTURE = 4000000000
ERR_SUCCESS = 0
ERR_NOT_FOUND = 2

# Calls the game only reports; the loader answers success without storing
# anything (its `reports[]` in namcoEs1Wmmt4Rpc.c).
REPORTS = {
    "update_user_lock",
    "register_system_stats",
    "register_ghost_trail",
    "save_charge",
    "save_multi_game_result",
    "update_team_stickers",
    "register_opponent_ghost",
    "lock_crown",
    "save_face_recognition_result",
    "save_car_campaign_info",
    "accept_car_campaign",
    "load_revenges",
}

# CarSetting.transmission: the loader writes 1 with the comment "manual", so
# 0 is automatic and this is what a car created as AT arrives with.
TRANSMISSION_AUTOMATIC = 0

# Car (v388.proto)
C_CAR_ID = 1
C_REGION_ID = 2
C_NAME = 3
C_SEARCH_CODE = 4
C_DEFAULT_COLOR = 8
C_LEVEL = 27
C_LAST_PLAYED_AT = 32
C_GHOST_LEVEL = 35
C_COUNTRY = 36
# Car in the stored message (LoadCarResponse)
L_ERROR = 1
L_CAR = 2
L_SETTING = 11

lock = threading.Lock()
users = None        # card -> {"id": int, "access": str, "cars": [ {...} ]}
next_user_id = None


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


# --- just enough protobuf -------------------------------------------------

def varint(value):
    out = bytearray()
    while True:
        byte = value & 0x7F
        value >>= 7
        out.append(byte | (0x80 if value else 0))
        if not value:
            return bytes(out)


def read_varint(data, index):
    value = 0
    shift = 0
    while True:
        byte = data[index]
        index += 1
        value |= (byte & 0x7F) << shift
        shift += 7
        if not byte & 0x80:
            return value, index


def parse_message(data):
    """field number -> list of values (int for varint, bytes for delimited)."""
    fields = {}
    index = 0
    try:
        while index < len(data):
            tag, index = read_varint(data, index)
            number, wire = tag >> 3, tag & 7
            if wire == 0:
                value, index = read_varint(data, index)
            elif wire == 2:
                size, index = read_varint(data, index)
                value = data[index:index + size]
                index += size
            else:
                break
            fields.setdefault(number, []).append(value)
    except IndexError:
        pass
    return fields


def uint_field(number, value):
    return varint(number << 3) + varint(value)


def bytes_field(number, data):
    return varint((number << 3) | 2) + varint(len(data)) + data


def text_field(fields, number):
    value = (fields.get(number) or [b""])[0]
    return value.decode("latin-1") if isinstance(value, bytes) else ""


def with_fields(raw, values):
    """raw message with `values` ({number: (wire, value)}) set, keeping order."""
    out, seen = bytearray(), set()
    index = 0
    try:
        while index < len(raw):
            tag, index = read_varint(raw, index)
            number, wire = tag >> 3, tag & 7
            if wire == 0:
                value, index = read_varint(raw, index)
            elif wire == 2:
                size, index = read_varint(raw, index)
                value = raw[index:index + size]
                index += size
            else:
                break
            if number in values:
                if number in seen:
                    continue
                wire, value = values[number]
                seen.add(number)
            out += uint_field(number, value) if wire == 0 else bytes_field(number, value)
    except IndexError:
        pass
    for number, (wire, value) in values.items():
        if number not in seen:
            out += uint_field(number, value) if wire == 0 else bytes_field(number, value)
    return bytes(out)


# --- users and their cars -------------------------------------------------

def store():
    """The card -> user table, read once from RPC_USERS (memory if unset)."""
    global users, next_user_id
    if users is None:
        users, next_user_id = {}, 1
        if USERS:
            try:
                with open(USERS) as handle:
                    saved = json.load(handle)
                users = saved.get("users") or {}
                next_user_id = int(saved.get("next_id") or 1)
            except (OSError, ValueError):
                pass
    return users


def save():
    if not USERS:
        return
    try:
        with open(USERS, "w") as handle:
            json.dump({"users": users, "next_id": next_user_id}, handle)
    except OSError as error:
        log(f"cannot write {USERS}: {error}")


def user_cars(record):
    return record.setdefault("cars", [])


def record_for_card(card, access):
    """The user for this card, created on first sight like the loader does."""
    global next_user_id
    table = store()
    record = table.get(card)
    if record is None:
        record = {"id": next_user_id, "access": access, "cars": []}
        next_user_id += 1
        table[card] = record
        save()
    record.setdefault("cars", [])
    return record


def record_for_user(user_id):
    for card, record in store().items():
        if int(record.get("id", -1)) == user_id:
            return card, record
    return None, None


def search_code(car_id):
    """Six characters, as the cabinet shows it (base36 of the car number)."""
    digits = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    out = ""
    value = car_id * 7919 + 104729
    for _ in range(6):
        out = digits[value % 36] + out
        value //= 36
    return out


# --- the calls ------------------------------------------------------------

def load_user(request):
    card = text_field(request, 1)
    access = text_field(request, 2)
    with lock:
        record = record_for_card(card, access)
        user_id = int(record["id"])
        existing = bool(record.get("cars")) or card in store()
        cars = list(record.get("cars") or [])
        access = record.get("access") or access
        save()

    message = (
        uint_field(1, ERR_SUCCESS)
        + uint_field(2, 0)
        + bytes_field(3, access.encode())
        + uint_field(4, user_id)
        + uint_field(5, user_id)
        + uint_field(6, user_id)
    )
    for car in cars:
        message += bytes_field(8, bytes.fromhex(car["car"]))
    for _ in cars:
        # has_opponent_ghost 0, COMPETITION_NOT_PARTICIPATED 1
        message += bytes_field(9, uint_field(1, 0) + uint_field(2, 1))
    if existing and not cars:
        message += bytes_field(9, uint_field(1, 0) + uint_field(2, 1))
    message += (
        uint_field(7, len(cars))
        + uint_field(10, 0)
        + uint_field(11, 0)
        + uint_field(12, 0)
    )
    if existing:
        message += uint_field(13, 1) + uint_field(14, 1)
    log(f"load_user: card {card} -> user {user_id} ({len(cars)} cars)")
    return message


def create_car(request):
    user_id = int((request.get(1) or [request.get(2, [0])[0]])[0])
    car_raw = (request.get(8) or [b""])[0]
    if not isinstance(car_raw, (bytes, bytearray)):
        car_raw = b""

    with lock:
        card, record = record_for_user(user_id)
        if record is None:
            log(f"create_car: no user {user_id}")
            return uint_field(1, ERR_NOT_FOUND)
        cars = user_cars(record)
        car_id = len(cars) + 1
        code = search_code(car_id)
        car = with_fields(
            car_raw,
            {
                C_CAR_ID: (0, car_id),
                C_REGION_ID: (0, REGION_ID),
                C_NAME: (2, CAR_NAME.encode()),
                C_SEARCH_CODE: (2, code.encode()),
                C_LEVEL: (0, 46),
                C_GHOST_LEVEL: (0, 1),
                C_LAST_PLAYED_AT: (0, int(time.time())),
                C_COUNTRY: (2, b"JPN"),
            },
        )
        # The car's own settings: view 2, transmission as the game sent it
        # (request field 9), then the loader's defaults.
        transmission = int((request.get(9) or [0])[0])
        setting = (
            uint_field(2, 2)
            + uint_field(3, transmission)
            + uint_field(4, 0)
            + uint_field(5, 0)
            + uint_field(6, 2)
            + uint_field(7, 0)
            + uint_field(8, 0)
            + uint_field(9, 0)
        )
        cars.append({
            "car_id": car_id,
            "car": car.hex(),
            "setting": setting.hex(),
            "search_code": code,
        })
        save()
        log(f"create_car: user {user_id} ({card}) car {car_id} \"{CAR_NAME}\" "
            f"model {int((parse_message(car).get(6) or [0])[0])} transmission {transmission}")

    return (
        uint_field(1, ERR_SUCCESS)
        + bytes_field(2, (record.get("access") or "").encode())
        + uint_field(3, user_id)
        + uint_field(4, user_id)
        + uint_field(5, user_id)
        + uint_field(6, car_id)
        + bytes_field(7, code.encode())
        + uint_field(8, 0)
        + uint_field(9, 0)
        + uint_field(10, 0)
        + uint_field(13, 0)
        + uint_field(14, 0)
    )


def load_car(request):
    car_id = int((request.get(1) or [0])[0])
    with lock:
        for record in store().values():
            for car in record.get("cars") or []:
                if int(car.get("car_id", -1)) == car_id:
                    message = uint_field(L_ERROR, ERR_SUCCESS) + bytes_field(
                        L_CAR, bytes.fromhex(car["car"]))
                    message += bytes_field(L_SETTING, bytes.fromhex(car["setting"]))
                    # Every other counter the game requires, at zero - the same
                    # sweep the loader does, plus the fields above 25 (the
                    # cabinet reported ERR_SUCCESS(incomplete) without them).
                    # 12 and 36..40 are not plain counters and stay out, and 23
                    # is the repeated rg_progress list.
                    for number in range(3, 65):
                        if number in (11, 12, 23) or 36 <= number <= 40:
                            continue
                        message += uint_field(number, 0)
                    for _ in range(50):
                        message += uint_field(23, 0)
                    log(f"load_car: car {car_id} returned")
                    return message
    log(f"load_car: car {car_id} not found")
    return uint_field(L_ERROR, ERR_NOT_FOUND)


def register_system_info():
    return (
        uint_field(1, ERR_SUCCESS)
        + uint_field(2, REGION_ID)
        + bytes_field(3, PLACE_ID.encode())
        + uint_field(4, 1300000000)
        + uint_field(5, 0)
        + uint_field(6, 0)
        + uint_field(7, FAR_FUTURE)
        + uint_field(8, FAR_FUTURE)
        + uint_field(9, FAR_FUTURE)
        + uint_field(10, FAR_FUTURE)
        + uint_field(11, FAR_FUTURE)
        + uint_field(12, 0)
        + uint_field(13, 1)
    )


def ping():
    return uint_field(1, ERR_SUCCESS) + uint_field(2, 1)


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.0"  # the cabinet's client is HTTP/1.0-era

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
        path = urlsplit(self.path).path
        name = path.rsplit("/", 1)[-1] or "root"
        log(f"{self.client_address[0]} {self.command} {path} {len(body)} bytes {body[:32].hex()}")

        if DUMP and body:
            try:
                os.makedirs(DUMP, exist_ok=True)
                with open(os.path.join(DUMP, f"{name}.pb"), "wb") as handle:
                    handle.write(body)
            except OSError as error:
                log(f"cannot dump {name}: {error}")

        if path.startswith("/method/"):
            request = parse_message(body)
            if name == "register_system_info":
                payload, status = register_system_info(), 200
            elif name == "ping":
                payload, status = ping(), 200
            elif name == "load_user":
                payload, status = load_user(request), 200
            elif name == "create_car":
                payload, status = create_car(request), 200
            elif name == "load_car":
                payload, status = load_car(request), 200
            elif name in REPORTS:
                # Report-only: nothing to store, success is the whole answer.
                payload, status = uint_field(1, ERR_SUCCESS), 200
            else:
                # Answered as "not found" on purpose: the cabinet then says
                # which call failed, which is the measurement we want.
                payload, status = uint_field(1, ERR_NOT_FOUND), 200
        elif path.startswith("/resource/"):
            payload, status = b"", 200
        else:
            payload, status = uint_field(1, ERR_NOT_FOUND), 404

        self.send_response(status)
        self.send_header("Content-Type", CONTENT_TYPE)
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)

    def log_message(self, fmt, *args):
        pass  # answer() logs


if __name__ == "__main__":
    log(f"listening on {BIND}:{PORT}, region {REGION_ID}, place {PLACE_ID}, "
        f"car name {CAR_NAME!r}, users {USERS or '(memory)'}, dumps {DUMP or '(off)'}")
    try:
        ThreadingHTTPServer((BIND, PORT), Handler).serve_forever()
    except KeyboardInterrupt:
        pass
