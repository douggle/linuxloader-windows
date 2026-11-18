# Project notes

## WMMT4 online target: the Banapassport service on douggle.us (user requirement, 2026-11-18)

The user wants the WMMT4 cabinet to connect to the Banapassport server on
**douggle.us**, and has approved using its literal IP, **40.160.55.11**, where
the game cannot resolve the name.

Measured facts (2026-11-18):

- `douggle.us` resolves to `40.160.55.11` (A record).
- Today the game is served **in-process** by the loader, not by any remote
  host: `WMMT4_SERVER_ADDRESS` is `"192.168.92.254"`
  (`src/loader/namco/namcoEs1Wmmt4Server.h:14`), every service name
  (`naominet.jp`, `v388-front.mucha-prd.nbgi-amnet.jp`, `*.loc`) is hijacked to
  that address by the `getaddrinfo`/`gethostbyname` hooks
  (`src/loader/namco/namcoEs1Wmmt4Net.c:67-79`), and `connect()` to that
  address is redirected to `127.0.0.1` (same file, line 92-97) where the
  loader's listeners answer ALL.Net (`PowerOn`), mucha (`boardauth.do`,
  `updatacheck.do`) and the game's protobuf RPCs (`docs/wmmt4.md`).
- `192.168.92.254` is not assigned to any interface on this machine
  (only 127.0.0.1, 192.168.1.29, 10.0.0.1, 192.168.1.127); the address only
  works because both the resolve and the connect are faked.
- `[Network] WMMT4_SERVER` (`src/loader/config/config.c:439`) is only a 0/1
  switch: `0` leaves the game offline. There is no host key, and
  `/mnt/Games2/wmmt4/linuxloader.ini` has no `[Network]` section.

Consequence: connecting the cabinet to douggle.us is not a config edit. It
needs either the hijack to return `40.160.55.11` with the `connect()` redirect
skipped (game talks to the remote directly, the loader's emulation becomes
unused), or the loader kept in front and made to forward to the remote. Note
that the third interception layer also has to change: curl itself is hooked
(`curl_easy_perform` in `namcoEs1Wmmt4Net.c`) and answers ALL.Net and Mucha
requests in the process, so a remote host needs those requests forwarded.

Port probe of 40.160.55.11 (2026-11-18, 3 s timeouts): **80 and 443 open**
(75 ms), **9001, 9002, 9000 time out**. So today the host answers web traffic
only; the arcade stack's ALL.Net port (9002) is not reachable from here, and
the game's server URI is `https://<host>:9002`.

Decision (user, 2026-11-18): **the loader forwards to it** — card reader and
local emulation stay.

Implemented: `WMMT4_SERVER_HOST` (env, `namcoEs1Wmmt4Net.c`). Set, the game's
connections to the services are made to that host instead of the loader's
listeners (`gameConnect`), and curl requests (ALL.Net, Mucha) are forwarded
with the game's OpenSSL as client (`forwardCurl`/`remoteExchange`); if the
remote does not answer, the loader's own answer is used so the cabinet still
boots. Unset, everything behaves as before.

Measured limit: an offline cabinet never sends anything, so forwarding is
unexercised — see below.
