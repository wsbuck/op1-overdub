# Memory map and boot chain (firmware 246, te-boot 2.30)

All addresses come from `tools/ldr_parse.py --summary` and decompilation of te-boot (`tools/decompile.sh`).
**Confirmed** = read directly from the binary. **Hypothesis** = inferred, not yet tested.

## Boot chain — **Confirmed** (te-boot init DXE, decompiled `FUN_ffa00000` in `te-boot_init.elf`)
1. The BF524 boot ROM boots the LDR stream from **NAND block 0** (te-boot.ldr).
2. **te-boot DXE 1 (init block)**, executed by the ROM:
   - sets up clocks and the TWI bus;
   - reads OTP page **0xD0** (XTEA key) and **0xD8** (provisioned flag) via the ROM `bfrom_OtpRead`;
   - scans the keys (`FUN_ffa0042c`).
3. If provisioned and **COM not held**, init patches the ROM boot struct:
   - source = **NAND block 4**;
   - callback = `0xFFA1007E`, the XTEA decrypt wrapper loaded at `0xFFA10000` (296 B).

   The ROM then boots **OP1_vdk.ldr** directly. te-boot's main code never runs.
4. If **COM is held**, the ROM continues into te-boot DXE 2: the menu, the USB disk updater, tests.
5. Consequence: recovery via COM depends only on NAND blocks 0–3 and OTP, never on the app image.

## NAND layout (Micron MT29F4G08, 128 KB blocks) — **Confirmed** (flasher.cpp / nand.cpp decompile)
| blocks | contents | written by |
|---|---|---|
| 0–3 (`< 4`) | te-boot.ldr | `FUN_00001068`. Runs only if the new te-boot lacks the string `te_boot_version:2.30`, **or** the update file name contains `FRC`. |
| 4 … (≤ 0x83) | OP1_vdk.ldr, raw | `FUN_00001388`: erases 128 blocks from 4, then writes 128 KB per block, skipping bad blocks |
| ≥ 0x80 … | YAFFS2 (`/yaffs2/…`: content, user data) | tar unpack |

- Boot-area write lock: `nand.cpp` asserts `block >= NUM_NAND_BOOT_BLOCKS (0x80) || !write_lock`. The flag is at `0xFF802C61`.

## Update path rules — **Confirmed** (te-boot `FUN_00000648`, firmware.cpp)
- In COM → 1 (upload), te-boot scans the USB disk for files. A file is skipped if its name starts with `.` or `_`, or its size is ≥ **0xF00000 (15,728,640 B)**.
- The file is read to SDRAM `0x01000000`. Its name is then checked by substring:
  - `.KEY` → **"Signature Key programming" (writes OTP — irreversible). Never use.**
  - `.OP1` → firmware package; only the first one is processed:
    - `FRC` in the name → force the te-boot rewrite. **Never use.**
    - `RBT` in the name → reboot when done.
- Update sequence (`FUN_0000efca`):
  1. CRC-32 check.
  2. Delete the old `/yaffs2/*.ldr`.
  3. LZMA + tar unpack.
  4. te-boot flasher (normally "skipped").
  5. "Writing firmware…" (app → block 4+).
- **Rule for our images:**
  - `te-boot.ldr` stays byte-identical to stock (enforced by `tools/op1pack.py`).
  - File names are short, uppercase-safe, and contain none of `FRC`, `RBT`, `KEY`.
  - Images stay under 15,728,640 B.

## RAM: what the app LDR loads — **Confirmed** (`ldr_parse.py --summary`)
| region | extent | contents |
|---|---|---|
| SDRAM | 0x01000000–0x01DFFF34 | code + data, plaintext (14.7 MB incl. zero-fill) |
| SDRAM | 0x02000000–0x02025800 | data (fill-heavy) |
| SDRAM | 0x02040000–0x027FFB04 | zero-fill (7.9 MB) |
| SDRAM | 0x03000000–0x0303FFFF | zero-fill (256 KB) |
| L1 data A | 0xFF800000–0xFF802FB4 | **encrypted** (and fill) |
| L1 data B | 0xFF900000–0xFF907E00 | **encrypted** (and fill) |
| L1 instr | 0xFFA00000–0xFFA0BFB4 | **encrypted**, 49,076 B, entry point (FINAL → 0xFFA00000) |

- The app never loads into: SDRAM below `0x01000000`, L1 data A ≥ `0xFF804000`, L1 instr ≥ `0xFFA10000`, or scratchpad.
- The init DXE's live state sits exactly in those places while the ROM loads the app:
  - XTEA callback at `0xFFA10000–0xFFA10128`;
  - key and state at `0xFF804000…`.
- **Our blocks must not touch** `0xFFA10000–0xFFA10128`, `0xFF804000–0xFF805FFF`, or the scratchpad. **Hypothesis** for the scratchpad: the ROM keeps its boot struct there.

## CPLB (MPU/cache) tables — **Confirmed** (L1 data A `0xFF802C3C…`, plaintext parts)
- Data CPLBs map all of SDRAM `0x00000000–0x03FFFFFF` in 4 MB pages:
  - `0x0–0x00FFFFFF`: flags `0x3109F` (locked, dirty);
  - the rest: `0x3101D` (WB cache);
  - `0x02400000` and `0x03400000`: `0x3501D` (write-through);
  - also the boot ROM.
- Instruction CPLBs (`0x31005`) cover SDRAM too.
- So code placed anywhere in SDRAM is executable. **SDRAM is 64 MB** (CPLB extent; matches the community spec).

## Candidate regions for our code
| candidate | size | confidence it's unused at runtime | notes |
|---|---|---|---|
| **A. iter's own code, overwritten in place** | iter's code size | High, if iter's code is in plaintext SDRAM and only runs when iter is selected | Preferred. Needs no new memory. Decided in Phase 2. |
| B. `0x01DFFF34–0x01FFFFFF` (gap after main image) | ~2 MB | Low–medium | One code reference to `0x01DFFFE4` (possibly a stack top). If used, stay well below that. |
| C. `0x00000000–0x00FFFFFF` | 16 MB | Unknown | Not loaded by the app. CPLB-locked (suggests active use, e.g. heap). te-boot uses it only on the COM path. |
| D. `0x027FFB04–0x02FFFFFF` | 8 MB | Unknown | 8 code constants point into it. |

- **Load check — Confirmed:**
  - Of the 3,778 distinct direct CALL targets from SDRAM code, 97.3% land on a function start (LINK/push, or right after RTS/JUMP).
  - The string `SELECT type FROM synth_types WHERE id=%i` (`0x018C7AEC`) is referenced from code at `0x01940356`.
  - te-boot's strings all resolve to the update/flasher code.

## Open questions
- Is the app's audio/engine render code in plaintext SDRAM or in the encrypted L1 instruction block? (Phase 2)
- Runtime heap and stack locations. These decide B, C and D. They would come from the VDK/runtime init, which starts at the encrypted entry point.
- Does the boot ROM accept a plaintext block appended after the encrypted ones? It should under standard BF52x semantics, but this is untested until Phase 3.
