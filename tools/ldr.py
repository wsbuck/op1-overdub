"""Blackfin BF52x LDR boot stream: parse and serialize.

Block header (16 bytes, little-endian):
  u32 block_code   bits 0-15 flags, 16-23 header checksum (XOR of all 16 bytes == 0), 24-31 0xAD
  u32 target_address
  u32 byte_count
  u32 argument
FILL blocks carry no payload; the 32-bit argument is the fill pattern.
"""
import dataclasses
import os
import struct
from dataclasses import dataclass

MAGIC = 0xAD

FLAGS = {
    0x0010: "SAFE",
    0x0020: "AUX",
    0x0100: "FILL",
    0x0200: "QUICKBOOT",
    0x0400: "CALLBACK",
    0x0800: "INIT",
    0x1000: "IGNORE",
    0x2000: "INDIRECT",
    0x4000: "FIRST",
    0x8000: "FINAL",
}
DMACODE_MASK = 0x000F
F_FILL, F_CALLBACK, F_INIT, F_IGNORE, F_FIRST, F_FINAL = 0x100, 0x400, 0x800, 0x1000, 0x4000, 0x8000

# BF52x memory regions (ADSP-BF52x hardware reference, "Memory map").
REGIONS = [
    (0x00000000, 0x04000000, "SDRAM (64 MB)"),
    (0x20000000, 0x20400000, "async banks"),
    (0xEF000000, 0xEF008000, "boot ROM"),
    (0xFF800000, 0xFF804000, "L1 data A SRAM"),
    (0xFF804000, 0xFF808000, "L1 data A SRAM/cache"),
    (0xFF900000, 0xFF904000, "L1 data B SRAM"),
    (0xFF904000, 0xFF908000, "L1 data B SRAM/cache"),
    (0xFFA00000, 0xFFA08000, "L1 instr bank A"),
    (0xFFA08000, 0xFFA0C000, "L1 instr bank B"),
    (0xFFA10000, 0xFFA14000, "L1 instr SRAM/cache"),
    (0xFFB00000, 0xFFB01000, "L1 scratchpad"),
]


def region_of(addr):
    for lo, hi, name in REGIONS:
        if lo <= addr < hi:
            return name
    return "UNMAPPED"


@dataclass
class Block:
    index: int
    offset: int  # file offset of header
    flags: int
    target: int
    count: int
    argument: int
    payload: bytes  # empty for FILL
    plain: bool = False  # payload already decrypted (see decrypt); not a flashable block any more

    @property
    def is_fill(self):
        return bool(self.flags & F_FILL)

    @property
    def is_encrypted(self):
        # OP-1: CALLBACK|INDIRECT blocks are XTEA-encrypted by TE (tabascoeye/op1-decryptor).
        return (self.flags & 0x2400) == 0x2400 and not self.plain

    @property
    def loads_memory(self):
        """True if this block writes target..target+count when loaded."""
        return self.count > 0 and not (self.flags & F_IGNORE)

    def flag_names(self):
        names = [n for bit, n in FLAGS.items() if self.flags & bit]
        unknown = self.flags & ~(DMACODE_MASK | sum(FLAGS))
        if unknown:
            names.append(f"?0x{unknown:x}")
        return "|".join(names) or "-"

    def header(self):
        code = self.flags | (MAGIC << 24)
        raw = struct.pack("<IIII", code, self.target, self.count, self.argument)
        chk = 0
        for b in raw:
            chk ^= b
        return struct.pack("<IIII", code | (chk << 16), self.target, self.count, self.argument)

    def serialize(self):
        return self.header() + self.payload


def parse(data):
    blocks, off = [], 0
    while off < len(data):
        if off + 16 > len(data):
            raise ValueError(f"truncated header at 0x{off:x}")
        raw = data[off:off + 16]
        code, target, count, arg = struct.unpack("<IIII", raw)
        if code >> 24 != MAGIC:
            raise ValueError(f"bad magic at 0x{off:x}")
        chk = 0
        for b in raw:
            chk ^= b
        if chk:
            raise ValueError(f"bad header checksum at 0x{off:x}")
        flags = code & 0xFFFF
        plen = 0 if flags & F_FILL else count
        payload = data[off + 16:off + 16 + plen]
        if len(payload) != plen:
            raise ValueError(f"truncated payload at 0x{off:x}")
        blocks.append(Block(len(blocks), off, flags, target, count, arg, payload))
        off += 16 + plen
    return blocks


def serialize(blocks):
    return b"".join(b.serialize() for b in blocks)


# XTEA, as applied by TE (tabascoeye/op1-decryptor): 32 cycles on the first 8 bytes of every 24-byte chunk,
# the other 16 bytes are plaintext. A trailing partial chunk of >= 8 bytes is encrypted too (30 of 46 blocks;
# e.g. 0xFF8017B4 decrypts to the screen geometry 320, 240, 320, 156), which op1-decryptor skips.
# Key = OTP page 0xD0, the same on every unit (verified 2026-10-06).
XTEA_DELTA = 0x9E3779B9
OTP_DUMP = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "third_party", "op1dumps",
                        "TE-OP-1-BF524-OTP-dump.txt")


def otp_key(path=OTP_DUMP):
    """Read the XTEA key (OTP page 0xD0) from the op1dumps OTP dump. The key is TE's: never commit it."""
    for line in open(path):
        f = line.split()
        if f and f[0].lstrip("!").lower() == "0xd0":
            return bytes.fromhex(f[1] + f[2])
    raise ValueError(f"no page 0xd0 in {path}")


def _xtea(data, key, enc):
    k = struct.unpack(">4I", key)
    out = bytearray(data)
    for i in range(0, len(out) - 7, 24):
        v0, v1 = struct.unpack_from("<2I", out, i)
        if enc:
            s = 0
            for _ in range(32):
                v0 = (v0 + ((((v1 << 4) ^ (v1 >> 5)) + v1) ^ (s + k[s & 3]))) & 0xFFFFFFFF
                s = (s + XTEA_DELTA) & 0xFFFFFFFF
                v1 = (v1 + ((((v0 << 4) ^ (v0 >> 5)) + v0) ^ (s + k[(s >> 11) & 3]))) & 0xFFFFFFFF
        else:
            s = (XTEA_DELTA * 32) & 0xFFFFFFFF
            for _ in range(32):
                v1 = (v1 - ((((v0 << 4) ^ (v0 >> 5)) + v0) ^ (s + k[(s >> 11) & 3]))) & 0xFFFFFFFF
                s = (s - XTEA_DELTA) & 0xFFFFFFFF
                v0 = (v0 - ((((v1 << 4) ^ (v1 >> 5)) + v1) ^ (s + k[s & 3]))) & 0xFFFFFFFF
        struct.pack_into("<2I", out, i, v0, v1)
    return bytes(out)


def decrypt(blocks, key):
    """Return blocks with every encrypted payload decrypted and marked plain (for analysis, not flashing)."""
    return [dataclasses.replace(b, payload=_xtea(b.payload, key, False), plain=True) if b.is_encrypted else b
            for b in blocks]


def encrypt_payload(payload, key):
    """Inverse of decrypt, for writing a modified payload back into an encrypted block."""
    return _xtea(payload, key, True)
