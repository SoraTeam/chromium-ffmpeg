#!/usr/bin/env python3
"""Patch per-distribution MTRR magics and P-256 public key into ffmpeg.dll.

Keep this file out of chromium/scripts/: that directory contains enum.py, which
shadows the stdlib if this script is launched from there.

The factory DLL contains a 1024-byte slot bounded by cookies that never change.
Runtime copies hdr/ftr UUID onto a 16KiB heap arena and seals JSON with
ephemeral P-256 ECDH + AES-256-GCM. AES key = SHA-256(ECDH x-coordinate),
matching BCryptDeriveKey(BCRYPT_KDF_HASH, SHA256).
"""
from __future__ import annotations

import argparse
import hashlib
import json
import struct
import sys
import uuid
from pathlib import Path

COOKIE_BEGIN = b"GPPMTRR1SLOTBEGN"
COOKIE_END = b"GPPMTRR1SLOTEND!"
SLOT_SIZE = 1024
SLOT_VERSION_OFF = 16
SLOT_PUBKEY_LEN_OFF = 20
SLOT_HDR_OFF = 24
SLOT_FTR_OFF = 40
SLOT_PUBKEY_OFF = 56
SLOT_PUBKEY_CAP = 640
SLOT_COOKIE_END_OFF = 696
ECC_PUB_LEN = 72
BCRYPT_ECDH_PUBLIC_P256_MAGIC = 0x314B4345

ARENA_SIZE = 16384
ARENA_JSON_LEN_OFF = 24
ARENA_EPH_OFF = 32
ARENA_IV_OFF = 104
ARENA_CT_OFF = 116
ARENA_TAG_LEN = 16
ARENA_FTR_OFF = 16368


def die(msg: str) -> None:
    raise SystemExit(msg)


def parse_uuid(text: str) -> bytes:
    try:
        return uuid.UUID(text).bytes
    except ValueError as exc:
        die(f"invalid UUID: {text} ({exc})")
    raise AssertionError


def find_slot(blob: bytes) -> int:
    hits = []
    start = 0
    while True:
        i = blob.find(COOKIE_BEGIN, start)
        if i < 0:
            break
        hits.append(i)
        start = i + 1
    if len(hits) != 1:
        die(f"expected exactly one slot cookie, found {len(hits)}")
    off = hits[0]
    if off + SLOT_SIZE > len(blob):
        die("slot cookie truncated")
    if blob[off + SLOT_COOKIE_END_OFF : off + SLOT_COOKIE_END_OFF + 16] != COOKIE_END:
        die("end cookie mismatch at expected offset")
    return off


def pubkey_blob_from_pem(path: Path) -> bytes:
    from cryptography.hazmat.primitives.asymmetric import ec
    from cryptography.hazmat.primitives.serialization import load_pem_public_key, load_pem_private_key

    data = path.read_bytes()
    try:
        key = load_pem_public_key(data)
    except ValueError:
        priv = load_pem_private_key(data, password=None)
        key = priv.public_key()
    if not isinstance(key, ec.EllipticCurvePublicKey):
        die("PEM is not an EC public key")
    if not isinstance(key.curve, ec.SECP256R1):
        die("expected P-256 (secp256r1) public key")
    nums = key.public_numbers()
    x = nums.x.to_bytes(32, "big")
    y = nums.y.to_bytes(32, "big")
    return struct.pack("<II", BCRYPT_ECDH_PUBLIC_P256_MAGIC, 32) + x + y


def slot_view(blob: bytes, off: int) -> dict:
    ver, pklen = struct.unpack_from("<II", blob, off + SLOT_VERSION_OFF)
    hdr = blob[off + SLOT_HDR_OFF : off + SLOT_HDR_OFF + 16]
    ftr = blob[off + SLOT_FTR_OFF : off + SLOT_FTR_OFF + 16]
    pub = blob[off + SLOT_PUBKEY_OFF : off + SLOT_PUBKEY_OFF + pklen]
    return {
        "offset": off,
        "version": ver,
        "pubkey_len": pklen,
        "hdr_uuid": str(uuid.UUID(bytes=hdr)),
        "ftr_uuid": str(uuid.UUID(bytes=ftr)),
        "pubkey_sha256": hashlib.sha256(pub).hexdigest() if pklen else "",
    }


def patch(dll: Path, hdr: bytes, ftr: bytes, pub: bytes, out: Path) -> None:
    data = bytearray(dll.read_bytes())
    off = find_slot(data)
    if len(pub) != ECC_PUB_LEN:
        die(f"public blob must be {ECC_PUB_LEN} bytes, got {len(pub)}")
    struct.pack_into("<I", data, off + SLOT_VERSION_OFF, 1)
    struct.pack_into("<I", data, off + SLOT_PUBKEY_LEN_OFF, ECC_PUB_LEN)
    data[off + SLOT_HDR_OFF : off + SLOT_HDR_OFF + 16] = hdr
    data[off + SLOT_FTR_OFF : off + SLOT_FTR_OFF + 16] = ftr
    data[off + SLOT_PUBKEY_OFF : off + SLOT_PUBKEY_OFF + SLOT_PUBKEY_CAP] = pub + b"\x00" * (
        SLOT_PUBKEY_CAP - len(pub)
    )
    if data[off : off + 16] != COOKIE_BEGIN:
        die("begin cookie clobbered")
    if data[off + SLOT_COOKIE_END_OFF : off + SLOT_COOKIE_END_OFF + 16] != COOKIE_END:
        die("end cookie clobbered")
    find_slot(bytes(data))
    out.write_bytes(data)
    print(json.dumps(slot_view(bytes(data), off), indent=2))


def decrypt_arena(arena: bytes, priv_pem: Path) -> None:
    from cryptography.hazmat.primitives.asymmetric import ec
    from cryptography.hazmat.primitives.ciphers.aead import AESGCM
    from cryptography.hazmat.primitives.serialization import load_pem_private_key

    if len(arena) < ARENA_SIZE:
        die(f"arena dump must be at least {ARENA_SIZE} bytes")
    json_len = struct.unpack_from("<I", arena, ARENA_JSON_LEN_OFF)[0]
    eph = arena[ARENA_EPH_OFF : ARENA_EPH_OFF + ECC_PUB_LEN]
    iv = arena[ARENA_IV_OFF : ARENA_IV_OFF + 12]
    ct = arena[ARENA_CT_OFF : ARENA_CT_OFF + json_len]
    tag = arena[ARENA_CT_OFF + json_len : ARENA_CT_OFF + json_len + ARENA_TAG_LEN]
    magic, cb = struct.unpack_from("<II", eph, 0)
    if magic != BCRYPT_ECDH_PUBLIC_P256_MAGIC or cb != 32:
        die("ephemeral key is not BCRYPT_ECCPUBLIC_BLOB P-256")
    x = int.from_bytes(eph[8:40], "big")
    y = int.from_bytes(eph[40:72], "big")
    eph_pub = ec.EllipticCurvePublicNumbers(x, y, ec.SECP256R1()).public_key()
    priv = load_pem_private_key(priv_pem.read_bytes(), password=None)
    shared = priv.exchange(ec.ECDH(), eph_pub)
    aes = hashlib.sha256(shared).digest()
    pt = AESGCM(aes).decrypt(iv, ct + tag, None)
    sys.stdout.write(pt.decode("utf-8") + "\n")


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--dll", type=Path, help="ffmpeg.dll to read or patch")
    ap.add_argument("--print", action="store_true", help="print the current slot")
    ap.add_argument("--hdr", help="header UUID to patch")
    ap.add_argument("--ftr", help="footer UUID to patch")
    ap.add_argument("--pubkey", type=Path, help="P-256 PEM (public or private)")
    ap.add_argument("--out", type=Path, help="patched DLL path")
    ap.add_argument("--decrypt", type=Path, help="16KiB arena dump to decrypt")
    ap.add_argument("--private", type=Path, help="P-256 private PEM for --decrypt")
    args = ap.parse_args()

    if args.decrypt:
        if not args.private:
            die("--decrypt requires --private")
        decrypt_arena(args.decrypt.read_bytes(), args.private)
        return
    if not args.dll:
        die("--dll is required")
    data = args.dll.read_bytes()
    off = find_slot(data)
    if args.print or not (args.hdr or args.ftr or args.pubkey):
        print(json.dumps(slot_view(data, off), indent=2))
        if not (args.hdr or args.ftr or args.pubkey):
            return
    if not (args.hdr and args.ftr and args.pubkey and args.out):
        die("patching requires --hdr --ftr --pubkey --out")
    patch(args.dll, parse_uuid(args.hdr), parse_uuid(args.ftr),
          pubkey_blob_from_pem(args.pubkey), args.out)


if __name__ == "__main__":
    main()
