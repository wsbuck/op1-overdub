# Firmware format (.op1)

## Stock images
| file | build string | size | sha256 |
|---|---|---|---|
| op1_246.op1 (latest) | `Rev. 00246; 2022/11/09 16:17:00; 8.1.11.1` | 13039128 | c5315218f825f143b415ca554516541898abee843d3a236df0b54c04e1fb13a9 |
| op1_245.op1 | `Rev. 00245; 2022/05/30` | 13027720 | d499f6a72dee13c9e7968c83ce3daefd780572020d5aacbda933bb79c7ba36d6 |

Source: https://teenage.engineering/_software/op-1/op1_246.op1 (and `_245`). Stored read-only in `firmware/stock/`.
**Confirmed** (sha256 recomputed 2026-10-06).

## Container — **Confirmed**
- Bytes 0–3: CRC-32 (zlib/IEEE), little-endian, over bytes 4..end. Recomputed for both images, matches.
- Bytes 4..: LZMA-alone stream, props 0x5d (lc=3 lp=0 pb=2), dict 8 MiB, size = unknown (0xff×8).
- Payload: GNU tar, regular files + dirs only:
  - `OP1_vdk.ldr` — application (2,171,624 B in 246)
  - `te-boot.ldr` — bootloader, `TE-BOOT v2.30` (261,604 B), byte-identical in 245 and 246
  - `content/` — SQLite DBs (`op1_factory.db`, `op1.db`, `kerntable.db`, `tape.db`), `display/*.svg`, `audio/`
- No signature: te-boot strings show only CRC-32 checking. **Hypothesis** until te-boot's update path is disassembled.
- 245 → 246: only `OP1_vdk.ldr` differs. **Confirmed** (file hashes).

## Install path — from te-boot strings
- te-boot checks CRC-32, then untars onto a YAFFS2 filesystem on NAND: `/yaffs2/OP1_vdk.ldr`, `/yaffs2/te-boot.ldr`. **Confirmed** (strings).
- te-boot then loads `/yaffs2/OP1_vdk.ldr` into RAM. **Hypothesis**.
  - If true, the application LDR never writes to flash, only to RAM, and our blocks would only reach RAM addresses.
- te-boot can rewrite itself into the NAND boot blocks: "Updating te-boot... / te_boot written. / te_boot skipped.", "version number %1.2f is too low.", and a `write_lock_enabled` guard on `NUM_NAND_BOOT_BLOCKS` in nand.cpp. **Confirmed** (strings).
  - The trigger is presumably a version comparison against `te_boot_version`. **Hypothesis**.

## Safety rule derived from this (applies to every image we build)
- `te-boot.ldr` must be byte-identical to stock (sha256 f6233b97…). The repack tool must refuse to build otherwise.
- Only `OP1_vdk.ldr` (and maybe `content/`) may change.

## iter / filter enable — **Confirmed** (op1repacker source + DB inspection)
- These are rows in `content/op1_factory.db`, not flags:
  - `synth_types`: `(11,'iter','[1516, 16704, 0, 15168, 0, 0, 0, 0]')`
  - `fx_types`: `(2,'filter','[7548, 0, 8272, 19572, 8000, 8000, 8000, 8000]')`
- `OP1_vdk.ldr` contains `iter`, `iter.svg`, `filter`, and `SELECT type FROM synth_types WHERE id=%i`.
- So engines are selected by DB type id. The id → code mapping lives in `OP1_vdk.ldr`.

## op1repacker
- Pinned at `third_party/op1repacker` @ 390b18e4. Stdlib Python; last code release v0.2.6 (2020).
- Used as a reference only. Issues with it:
  - Repacks with LZMA lp=1 (stock uses lp=0).
  - Tar entries come out unsorted.
  - No te-boot or size checks.
  - Its comment claims a ~15 MB device limit. **Hypothesis**.
- We'll write our own deterministic `tools/op1pack.py`.

## Encryption in OP1_vdk.ldr — **Confirmed** (block scan of 246, 2026-10-06)
- Blocks with flags `0x2400` (BFLAG_CALLBACK + …) are XTEA-encrypted per tabascoeye/op1-decryptor:
  - In every 24-byte chunk, the first 8 bytes are encrypted (32 cycles) and the next 16 are plaintext.
  - The key is OTP page 0xD0, in the byte order the op1dumps dump prints it. **Confirmed** (2026-10-06):
    the key from that dump (another unit, fw 241 era) decrypts our stock 246. Decrypted 8-byte chunks have
    13.0% zero bytes vs 12.6% in the plaintext 16 bytes (0.3% before decryption, and with the other 3 byte
    orders). objdump of L1 instr: 3,083 illegal instructions → 2. So the key is per model, not per unit.
  - `ldr.otp_key()` reads it from `third_party/op1dumps`; `ldr.decrypt()` / `ldr.encrypt_payload()`
    (Python, byte-identical to tabascoeye's C tool, round-trips). The key is TE's: never commit it.
- Scan of 246 (801 blocks total):

  | kind | blocks | bytes |
  |---|---|---|
  | plaintext data | 342 | 2,078,244 |
  | fill/zero | 413 | 21,155,767 |
  | encrypted | 46 | 80,564 |

- Every encrypted block targets on-chip L1:
  - L1 data bank A: `0xFF800000…`
  - L1 data bank B: `0xFF900000…`
  - L1 instruction SRAM: one 49,076 B block at `0xFFA00000`
- All SDRAM code/data (`0x01000000…0x01Axxxxx`) is plaintext.
- Implication: hot DSP loops are probably in the encrypted L1 instruction block. Tables and most code in SDRAM are readable without a key.
- te-boot.ldr has no encrypted blocks.
- Public reference: `third_party/op1dumps` (@a353df4) has a BF524 OTP dump including page 0xD0 (the key).
