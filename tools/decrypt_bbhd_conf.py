#!/usr/bin/env python3
"""Decrypt Big Buck HD Wild's encrypted prod/conf data files offline.

The g5 engine encrypts each data file as AES-256-CBC(zlib(text)), with a
per-file key and IV derived from the file's cabinet path ("/pm/" + the path
relative to the game root) and two 'salt' buffers that come from the dongle at
startup.  Those two salts are exactly the recorded dongle answers already kept
in the game's hasp/ folder:

    salt512 (512 B) = hasp/3afb28b9b837b0f9d3c96373602d0d9e   (AES key material)
    salt256 (256 B) = hasp/0f8b5fdb2accd0ddea416a7f1d4b0c48   (AES IV  material)

So no dongle and no per-file recording is needed: every conf file decrypts from
data already on disk.  (The efilemap index files use a dongle-wrapped key and
are NOT needed -- the data files carry their real names.)

Key/IV derivation is the game's own routine at 0x83078ad (a deterministic
RC4-like mixer of salt and name).  Pipeline: AES-256-CBC decrypt, then zlib
inflate.

Usage:
    decrypt_bbhd_conf.py GAME_DIR [-o OUT_DIR] [--hasp DIR] [--check]

  GAME_DIR   the game root (the folder that contains g5/)
  -o OUT_DIR write each decrypted file there, mirroring the tree (default: none,
             just report)
  --hasp     folder holding the two salt answer files (default: GAME_DIR/hasp)
  --check    only count how many files decrypt, print a few samples
"""
import argparse, os, sys, zlib
try:
    from Crypto.Cipher import AES
except ImportError:
    sys.exit("needs pycryptodome:  pip install pycryptodome")

SALT512 = "3afb28b9b837b0f9d3c96373602d0d9e"
SALT256 = "0f8b5fdb2accd0ddea416a7f1d4b0c48"
MOUNT = "/pm/"


def kdf(salt, name, outlen):
    """The game's key-derivation at 0x83078ad: mix salt and name bytes."""
    nb = name.encode()
    sl, nl = len(salt), len(nb)
    if sl == 0 or nl == 0:
        return b""
    i0 = (outlen + nl) % sl
    i1 = (outlen + sl) % nl
    out = bytearray()
    for k in range(outlen):
        out.append(salt[i0] ^ nb[i1])
        i0 = (i0 + nb[i1] + k + outlen) % sl
        i1 = (i1 + salt[i0] + k + outlen) % nl
    return bytes(out)


def decrypt(ciphertext, cab_path, salt512, salt256):
    """Return the inflated plaintext, or None if this isn't an encrypted file."""
    if len(ciphertext) < 16 or len(ciphertext) % 16:
        return None
    key = kdf(salt512, cab_path, 32)
    iv = kdf(salt256, cab_path, 16)
    plain = AES.new(key, AES.MODE_CBC, iv).decrypt(ciphertext)
    try:
        return zlib.decompress(plain)
    except zlib.error:
        return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("game_dir")
    ap.add_argument("-o", "--out")
    ap.add_argument("--hasp")
    ap.add_argument("--check", action="store_true")
    args = ap.parse_args()

    hasp = args.hasp or os.path.join(args.game_dir, "hasp")
    salt512 = open(os.path.join(hasp, SALT512), "rb").read()
    salt256 = open(os.path.join(hasp, SALT256), "rb").read()
    if len(salt512) != 512 or len(salt256) != 256:
        sys.exit(f"bad salts: {len(salt512)}/{len(salt256)} (want 512/256)")

    ok = skip = 0
    samples = []
    for root, _dirs, files in os.walk(args.game_dir):
        for fn in files:
            path = os.path.join(root, fn)
            rel = os.path.relpath(path, args.game_dir).replace(os.sep, "/")
            cab = MOUNT + rel
            try:
                ct = open(path, "rb").read()
            except OSError:
                continue
            plain = decrypt(ct, cab, salt512, salt256)
            if plain is None:
                skip += 1
                continue
            ok += 1
            if len(samples) < 4:
                samples.append((rel, plain[:80]))
            if args.out:
                dst = os.path.join(args.out, rel)
                os.makedirs(os.path.dirname(dst), exist_ok=True)
                with open(dst, "wb") as f:
                    f.write(plain)

    print(f"decrypted {ok} files" + (f", wrote to {args.out}" if args.out else "")
          + f"   (skipped {skip} non-encrypted/other)")
    for rel, head in samples:
        print(f"\n--- {rel} ---\n{head.decode('latin-1').strip()}")


if __name__ == "__main__":
    main()
