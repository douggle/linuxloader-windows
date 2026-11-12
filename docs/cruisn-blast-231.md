# Cruis'n Blast 2.31: the Sentinel LDK gate

[hasp-bypass.md](hasp-bypass.md) covers games whose protection is the HASP API
layer. Cruis'n Blast 2.31 is not one of them in practice. The loader identifies
the dump, installs every Raw Thrills and HASP entry point, and the game still
exits before a single one of them is called, because the executable is wrapped
in a **Sentinel LDK Envelope** that decides before the game's own code runs.

This is what the envelope does, established from the running process. It is not
a bypass; it is the map of what would have to be bypassed.

## The dump

32-bit i386, non-PIE, 35,634,303 bytes. Full-file CRC32 `0xc19fbb51`, partial
`0x53bf0fbf`. `LOAD` segments at `0x08048000` and `0x08ab6f90`; file offset is
`vaddr - 0x08049000` throughout the second segment.

The ELF entry is `0x09f1cf04`, which is inside the envelope, not the C runtime.
That is the first sign that this dump is not "the game".

## The shape of it

The envelope's init at `0x09ef1163` enters its licence engine indirectly through
a single table slot, `0xa2024a8`, which has five callers (`0x09ef1173`,
`0x09ef127e`, `0x09ef16d9`, and two more). The engine forks a worker, and the
two processes talk over a small shared-memory area protected by a named
semaphore. If the worker does not deliver a licence, the parent calls one of two
obfuscated fatal helpers (`0x09f018c0`, `0x09f01900`), which always `_exit(1)`
regardless of their argument.

## The shared memory is a log queue, not a verdict

`shmget()` is called with a key derived from a runtime pointer (it changes every
run: `0x1037f7`, `0x1041f6`, ...), size 436, followed by `shmat()` and
`sem_init(pshared=1)`. The producer is `fcn.09f0cb00`:

```
0x09f0cb9c  call __sprintf_chk      ; builds a semaphore name at ebx+0x2ee1c
0x09f0cbbc  call sem_open
0x09f0cbfd  call sem_wait
0x09f0cc12  lea  eax, [ebx+0x16ab8] ; queue head pointer, at 0x0a20340c
0x09f0cc1a  mov  ecx, [edx]         ; [head] = message count
0x09f0cc23  mov  [eax+ecx*4+4], ebp ; push the message
0x09f0cc2a  call sem_post
0x09f0cc36  call shmdt
```

The same `lea eax, [ebx+0x16ab8]` appears at thirty-odd sites across the
envelope. It is a general-purpose internal message channel, and the bytes that
land at `+32`, `+36`, `+40` in a dump of the segment are pushed payload, not
state. A pause-and-dump of the segment therefore shows a count incrementing
(`+36: 01 -> 02`) while the same worker writes `byte[i] = i` across the block as
a teardown canary. Reading the segment's "state" as a licence result is a dead
end; so is patching it.

## The licence probe is a USB control transfer

The engine enumerates `/sys/bus/usb/devices/*`, reading `idVendor` and then
`idProduct` through `fcn.0a19339c` (an `open`/`read`/`close` helper that strips
trailing whitespace via `__ctype_b_loc`). It is looking for vendor `0x0529`,
the Sentinel HL family.

When a device matches, `fcn.0a1932f0` issues the query:

```
0x0a193319  mov byte [arg+0x18], 0xa1      ; IN | class | interface
0x0a19331e  mov byte [arg+0x19], 0x01      ; bRequest
0x0a193323  mov word [arg+0x1a], 0x0300    ; wValue
0x0a19334d  mov dword [esp+4], 0xc0105500  ; USBDEVFS_CONTROL
0x0a193355  call ioctl
0x0a19335a  sar  eax, 0x1f                 ; -1 or 0
```

This is the v2c licence request to the key itself. Nothing downstream of it is a
boolean that a loader can set: whatever follows parses the key's answer.

Pretending a key is present at the sysfs level (rewriting every `idVendor` read
to `0529`, which reaches all eighteen probed devices) does move the engine
forward, to `idProduct` on each device, and then the run still fails, because
there is no `/dev/bus/usb/...` node behind the impostor and the ioctl has
nothing to talk to.

## Hardware fingerprinting

Before and around the probe the engine reads:

- `/proc/self/mountinfo` (fourteen reads), `/sys/block/sd*/{dev,model,removable}`
  for several disks, and `/dev/sda`;
- `/var/hasplm/hasplm.version`, which does not exist on a machine without the
  LDK runtime installed;
- `~/.hasplm/fs/<id>/` — `.lock`, `.token` (four bytes, `01 00 00 00`) and a
  `.id/` directory of hashed marker files. The id is not the pid.

The binary's own strings confirm the mechanism: `Failed to write fridge token`,
`Failed to retrieve list of connected Sentinel HL keys`,
`<hasp configuration="sentinelhl,driverless" />`, `/dev/aks/sentinelhid`.

## The supervisor

The parent is `fcn.09f5600`, a 10,849-byte flattened state machine. Its retry is
one instruction pair, reached after `sleep(1)`:

```
0x09ef56e5  call sleep
0x09ef5704  cmp  dword [ebx+0x2ef20], 0     ; global 0x0a250c74
0x09ef5707  je   ...
```

No instruction in the image writes `[ebx+0x2ef20]` directly; the slot is filled
through the obfuscated dispatcher. When the budget expires the supervisor takes
the fatal branch at `0x09ef62ff`.

## What has been tried

| Change | Result |
|---|---|
| Loader's HASP layer (login/read/write/encrypt/decrypt, dongle model of 1.25) | installed and correct; never called |
| NOP the fatal call sites `0x09ef56c1`, `0x09ef56d3`, `0x09ef62ff` | no exit, but the parent sleeps in the retry loop forever |
| NOP the engine call in envelope init, `0x09ef1173` | `SIGILL`, the call bootstraps the envelope |
| Force the retry global `0x0a250c74 = 1` every 20 ms | unchanged, exits 1 |
| Prefill the 676 game-import slots (`cbImports231.h`) | `free(): invalid pointer`, or no effect |
| Rewrite `idVendor` to `0529` for every probed device | enumeration reaches `idProduct`, still exits 1 |

## What this means for the loader

Nothing in `rtGames.c` or `rtDongle.c` can reach this gate: it closes before the
game's imports are resolved, so 1.25's dongle answers are never consulted. The
two routes that remain are a real Sentinel HL key (or an installed hasplm
licence manager holding a v2c licence for this feature), or defeating the
envelope in the binary itself.

Big Buck Hunter Pro is the one precedent for doing the latter: its Rockey boot
envelope is a fixed 32-bit XOR mask, so the launcher restores the code in a
cached copy and starts at the restored entry, past both the checks and the
watchdog (see [big-buck-hunter-pro.md](big-buck-hunter-pro.md)). Cruis'n Blast
2.31 is not that: there is no fixed mask, the engine's decision lives in packed
code, and the probe ends in a device transaction rather than a comparison.

## Tooling

The probes used here are `LD_PRELOAD` shims, all of which are disposable:

- `lic_probe.so` — logs `read`/`setsockopt`/`sem_init`/`sem_wait`/`sem_post`/
  `shmget`/`shmat`, diffs the 436-byte segment, and dumps it at `exit`;
- `hlbt.so` — backtrace on `open` of any `usb/devices`, `idVendor`, `hasplm` or
  `/sys/bus/pci` path, which is how `fcn.0a19339c` was located;
- `fakevid.so` — rewrites `idVendor` reads to `0529`;
- `dumpmaps.so` — writes every executable mapping out at exit, for Ghidra.

They run ahead of the loader, e.g.

```
LD_PRELOAD="<shim>.so linuxloader.so" LD_LIBRARY_PATH=./utils:. ./game
```

Ghidra analysis of the dumped mappings showed that the large anonymous regions
are data (0 functions) and the only code among them belongs to `libpng16` and
`libfreetype`; the envelope's own code is entirely in the mapped ELF, at
`0x09ef0000`, `0x09f0c000` and `0x0a193000`.
