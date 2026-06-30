#!/usr/bin/env python3
"""Executable spec + regression guard for apd's module-config wire format.

Mirrors apdc/cpp/module_config.cpp (and ../apd/src/module_config.rs):
    magic "APTM"(LE u32 0x4150544D) | version(LE u32 1) | count(LE u32)
    count * { keyLen(LE u32) | key | valLen(LE u32) | val }   (UTF-8, LE u32)

Run on the host now to pin the format; reuse on-device to inspect/validate a
real persist.config the daemon writes. No deps, no framework.

    python test_config_format.py
"""
import struct

MAGIC = 0x4150544D
VERSION = 1


def encode(config: dict[str, str]) -> bytes:
    out = struct.pack("<III", MAGIC, VERSION, len(config))
    for k, v in config.items():
        kb, vb = k.encode("utf-8"), v.encode("utf-8")
        out += struct.pack("<I", len(kb)) + kb + struct.pack("<I", len(vb)) + vb
    return out


def decode(data: bytes) -> dict[str, str]:
    magic, version, count = struct.unpack_from("<III", data, 0)
    if magic != MAGIC:
        raise ValueError(f"bad magic 0x{magic:08x}")
    if version != VERSION:
        raise ValueError(f"bad version {version}")
    off, cfg = 12, {}
    for _ in range(count):
        (klen,) = struct.unpack_from("<I", data, off); off += 4
        key = data[off:off + klen].decode("utf-8"); off += klen
        (vlen,) = struct.unpack_from("<I", data, off); off += 4
        val = data[off:off + vlen].decode("utf-8"); off += vlen
        cfg[key] = val
    return cfg


def main() -> None:
    # round-trip, incl. empty value and unicode
    sample = {"enabled": "true", "override.description": "你好 🌶", "empty": ""}
    blob = encode(sample)
    assert decode(blob) == sample, "round-trip mismatch"

    # exact header bytes: magic LE = 4D 54 50 41, version = 01 00 00 00
    assert blob[:8] == bytes([0x4D, 0x54, 0x50, 0x41, 0x01, 0x00, 0x00, 0x00]), blob[:8].hex()

    # empty config => header with count 0
    assert encode({}) == struct.pack("<III", MAGIC, VERSION, 0)
    assert decode(encode({})) == {}

    # corruption rejected
    bad = bytearray(blob); bad[0] ^= 0xFF
    try:
        decode(bytes(bad)); raise SystemExit("FAIL: bad magic not rejected")
    except ValueError:
        pass

    print("ok: config wire format round-trips and header is stable")


if __name__ == "__main__":
    main()
