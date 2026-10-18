# Kensington VeriMark Desktop 2.0 (047d:8228) — Linux protocol findings

Date: 2026-10-17
Host: CachyOS, libfprint 1.94.100 + modified upstream `realtek` driver
Device: Kensington VeriMark Desktop 2.0 (K64741WW), USB `047d:8228`
"Kensington fingerprint device", iSerial 201801010001, bcdDevice 0.13
Interface: vendor-class, subclass 2, single interface.
Endpoints: EP 0x01 bulk OUT (512B), EP 0x82 bulk IN (512B), EP 0x83 interrupt IN (64B).
Windows driver: `kensingtonmocwbdi.inf` (AtansMocWbdi.dll / AtansMocAdapter.dll /
AtansMocUtil.dll, Realtek "rtswbdi" project) — same protocol family as
Realtek/Microctopus `2541:fa03` and stock `0bda:5813/5816`.

## Command transport (12-byte bulk frames on EP 0x01)

struct { u8 class; u8 sub; u8 param[4]; u8 addr[4]; u16le data_len; }
class bits 7:6 = command type: 0 = bulk-only, 1 = read, 2 = write.
Replies: bulk-IN data phase (data_len bytes for reads), then a 5-byte status
frame `00 00 00 00 00` on success.

## Confirmed working commands

| Frame | Name | Notes |
|---|---|---|
| `05 13 01 …` | select_os (mode 1) | accepted |
| `05 13 00 …` | select_os (mode 0) | accepted; changes DB semantics (see below) |
| `45 0d` dlen=2 | get_enroll_num | always replies `0d 0a` (10 = slot count, 0x0d unknown) |
| `45 0e` dlen=35×N | get_template | returns N×35 B records, all-zero when empty |
| `05 20` param=slot | begin_pos | status 00 |
| `05 05` | start_capture | status 00 |
| `45 06` dlen=5 | finish_capture (poll) | data `01 xx xx 01 00` = not captured; `00 …` = captured |
| `45 08` param=0x04 dlen=9 | accept_sample (enroll) | 9 B reply `00 <6B>`, status 00 = sample accepted |
| `45 10` dlen=34 | check_duplicate | data[0]: 0x0b = MATCH_FAIL (no dup), 0x00 = duplicate, 0x0c = error |
| `05 07` param=0x04 | cancel_capture | status 00 |
| `05 0f` param=slot (0-9) | delete_record | status 00; slots ≥ 10 → device goes idle w/o reply |
| `05 0f` param=0xff | clear storage | status 00 |

Capture quality codes (accept/finish replies): 0=OK 1=TOO_HIGH 2=TOO_LOW
… 11=MATCH_FAIL 12=CMD_ERR (same enum as upstream libfprint realtek driver).

## Idle heartbeat — critical semantic

`01 F7 FF FF FF` is pushed **spontaneously** on EP 0x82 whenever the sensor
finishes processing (also right after every USB reset). It is **not** an error
reply. A status read that lands after the sensor went idle catches the
heartbeat instead of a command status. Confirmed identical on the sibling
Microctopus 2541:fa03 device (romainframe RE notes).

## select_os modes (template database selector)

- Mode 1 (stock libfprint driver value): database-class commands
  (check_duplicate status read, commit) never return a real status — only the
  idle heartbeat. Captures/capturing work normally.
- Mode 0: check_duplicate returns a real reply (status 00, 34 B data filled
  0xFF = no match). Full capture session (begin_pos/capture/poll/cancel)
  verified working in mode 0.
- Interpretation: mode = on-device template database selector (compare the
  SDCP research: MOC sensors keep separate per-driver databases chosen by an
  unauthenticated init packet). Mode 1 = Windows Hello SDCP database;
  unauthenticated operations are discarded. Mode 0 = open database.

## UNSOLVED: template persistence (enroll commit)

The stock commit `85 21 param=0xFF dlen=32 + 32 B uid payload` never results
in a stored template on this firmware (verified by raw `45 0e` reads staying
all-zero at any length, and `45 0d` count unchanged). Tried wire variants,
all without effect:
- 85 21 param 0xFF / 0x00(pos) × dlen 32/28/35(record-shaped) — heartbeat or
  ACK, DB unchanged.
- 85 21 dlen=0 (no payload) — clean status 00, DB unchanged.
- 85 22, 85 20 payloads — same.
- 05 11 (update_template) — triggers a ~4 s flash operation and a firmware
  reboot, and ERASES stored data; never store the session template. Do not
  send during enrollment.
- select_os mode 0 before/around commit — no change.

Slot deletes (05 0f) on slots 0-9 succeed and 10+ go idle → 10 slots total,
matching the `0a` in the count reply.

Device-info control read (bRequest 0x07, wValue 0x000D, 8 B):
`39 00 00 00 FF FF FF FF` — 0x39 is NOT the template record length (records
are 35 B as in stock).

## What works end-to-end today

Device detection, open/init (with the reset-greeting drain), full 8-stage
enrollment capture pipeline with quality feedback, duplicate detection,
storage enumeration semantics. Missing: the final template commit so that
enrollments persist across sessions.

## Artifacts

- Patched libfprint tree: `/tmp/opencode/verimark/libfprint` (ID table
  +047d:8228, CMD/STATUS timeouts raised, reset-greeting drain, commit
  variant cascade, select_os mode experiments). Build: `builddir/`.
- Probe/sweep tooling: `/tmp/opencode/verimark/{probe,probe4,sweep}.py`.
- usbmon captures: `/tmp/opencode/verimark/mon5*.bin` + parser
  `parse_mon.py` (record layout: 48 B header + len_cap data; INT records 56 B).
- Windows driver (AtansMoc): `/tmp/opencode/verimark/windrv/` (extracted from
  Microsoft Update Catalog, update 0a782f36-3945-4f08-9e29-181178933161).

## Addendum (2026-10-17, later session)

Additional valid 05-class opcodes discovered by in-session sub-opcode sweep
(all ACK cleanly): `05 12`, `05 15`, `05 16` (besides the known 05 05/07/0f/
11/13/20). Reply semantics during an ACTIVE session: real ACKs are
`00 00 00 00 00` or `00 f7 ff ff ff` (status 00 + device marker); the idle
heartbeat `01 f7 ff ff ff` appears only when the sensor has gone idle.

- `05 12` sent WITH a 32 B payload causes the device to stream zero-filled
  data (2048+ B observed) and then stop replying (wedge) — it is a data-
  transfer class command, not the enrollment commit.
- 05 12 / 05 15 / 05 16 with param=0x00, payload-less: ACK only, no storage.
- Exhaustive in-session sweeps: 85-class subs 0x00-0x40 all ACK `00 f7 ff ff ff`
  during a session; 05-class subs as above. 45-class reads with dlen=0 stall
  (0-byte data phase) — must use bwait_data_delay=0 to avoid an infinite read.
- After all of the above, the template database (45 0e read, both select_os
  modes) remains all-zero; `45 0d` count stays `0d 0a`. Template persistence
  remains UNSOLVED. Next step: full RE of the Wbdi.dll ioctl→USB translation
  layer (adapter issues ioctl 0x440004 in SensorAdapterSetMode), or SDCP-
  session analysis (device-info `39 00 00 00 ff ff ff ff` may gate DB ops).

## Addendum 2 — Deep RE round 2 (Ghidra decompilation of AtansMocWbdi.dll + AtansMocAdapter.dll)

Tools: Ghidra 12.1.4 headless (portable JDK 21) — full decompilation of both
DLLs (1184 + 3016 functions) in ~/verimark/{wbdi,adapter}_decomp.c.

### WBDI driver ioctl dispatch (device 0x44, CTL_CODE func*4)
0x440004→FUN_18000761c, 0x440008→FUN_180009a80, 0x44000c→FUN_180005e98,
0x440010→FUN_180007e08, 0x440014→FUN_1800060f4, 0x442000/0x4430xx→vendor range
handlers FUN_180005e98..FUN_180009a80. Ioctls 0x44300c-0x44301c dispatch through
a stored callback (DAT_180064e80).

### Driver op interface
Session object constructed by FUN_18003c8f8 (vtable PTR_FUN_180055c08, fields:
+0x10=10 (slots), +0x21=0xFF (SUB_FINGER), +0x2c=ctx byte). Vtable slots:
slot3=FUN_18003e0d0, slot4=FUN_18003cf30 (COMMIT), slot5=FUN_18003d500, ...

### USB transport (FUN_18003e9b4) — byte-identical to our driver's flow
write cmd(12B) → if mode 0x10: write payload / if mode 0x20: read payload →
read 5B status. Commit wrapper FUN_18003cf30: requires dlen==32, cmd
`85 21 param2 addr=0 dlen=32`, mode 0x10. IDENTICAL to what we send.

### Windows capture flow differs from the upstream Linux driver!
- Capture data read = `45 06 [purpose] [ctx+0x2c]` + READ 1-5 bytes
  (FUN_18003d0c4; reply first byte 00 = data valid). ONE command per
  FinishCapture — the upstream driver's separate plain-45 06 poll +
  45 08 accept_sample (9 B) pair is NOT what the Windows driver sends.
- NO 45 08 (accept_sample), 45 0e (get_template) or 45 10 (check_duplicate)
  command constructions exist anywhere in AtansMocWbdi.dll.
- 45 22 (identify, vtable slot5 FUN_18003d500): cmd[3]=(session!=0)+1,
  cmd[8..9] = u16 from WTSGetActiveConsoleSessionId/WTSQuerySessionInformationW
  (FUN_18003d614) — operations are bound to the Windows session id.

### The enrollment record flows through a named pipe
0x44307c handler (FUN_180008c74): when the capture result's first byte is 0x01,
the driver creates `\\.\Pipe\EnrollRecordPipe` ( duplex, 20000-byte buffers,
handle stored in ctx+0xb2); result byte 0x02 = enrollment finished → pipe torn
down. The pipe name exists ONLY in the driver (no client in the adapter/util
DLLs) — the client is the WinBio engine side. The 20 kB buffer size matches a
full template record.

### Working hypothesis (updated)
The commit `85 21` only labels the stored template with a 32-byte identity
(= the engine's identity hash; host records are 35 B with the 32-byte identity
at +3, compared against FUN_1800227a0(identity32) during verify). The template
feature data itself must be uploaded to the sensor by a separate bulk-write
command (large dlen, fed from the EnrollRecordPipe) that we have never sent.
Candidates: an 85-class sub with large dlen, or 05 12/15/16 with a
correctly-framed data phase. The "zero stream" the device emitted on malformed
data-phase commands (05 12+32B, 85 21 dlen=0) suggests data-transfer commands
exist and behave differently from plain ACKs.

## Addendum 3 — THE STORE COMMAND FOUND

Systematic command-construction sweep of wbdi_decomp.c revealed commands the
upstream driver never sends:

- **`85 0b` dlen=32 (params all zero) + 32 B payload = THE TEMPLATE STORE**
  (FUN_18003e0d0, op-table slot 3). Clean status 00. This is the command the
  whole investigation was missing — `85 21` belongs to a different protocol
  variant.
- **`45 0c` dlen=0x4a** = 74-byte record readback (cmd[3] = session flag byte,
  1 or 2). Reply: byte[0] = record state (0x0b right after a store attempt,
  0x09 on a fresh session), bytes[66..73] = stable metadata counters
  (e.g. 36 22 be 00 47 02 00 00). The 74-byte record is the on-device
  enrollment record.
- `45 07`, `86 01`, `86 03` also constructed by the Windows driver (untested).
- FUN_18003d614: session binding via WTSGetActiveConsoleSessionId +
  WTSQuerySessionInformationW(0, sess, 0x19, ...) — the value feeds cmd[8..9]
  of 45 22 and the flag byte of 45 0c.
- Enrollment record pipe protocol (FUN_180008ed4, ioctl 0x443038): capture
  results are NINE bytes (identical to our 45 08 replies); gated on
  purpose==4; each result is framed (2 ctx bytes @0x59d/0x59e + 8 B result +
  1 B + 4 B @0x599, total +15) and appended to `\\.\Pipe\EnrollRecordPipe`.
  Stage counter ctx+0x52 vs expected ctx+0x2c5.

### Current status
`85 0b` + 32 B payload ACKs cleanly and the 45 0c readback changes state, but
the identity body (record[1..65]) stays zero — the payload content/format is
still not what the engine sends. The engine hashes identities with an mbedTLS
context type 9 (FUN_1800227a0 → FUN_18003d740(9) → FUN_18003c9d0) and stores
hash(identity32) at record+3 (verified via the memcmp in the verify path).
Next: identify the type-9 hash (likely SHA-256/HMAC — dump FUN_18003c9d0),
or find the caller of op-slot 3 to see the exact payload bytes.

## Addendum 4 — deeper driver decode (same session)

- **Template-list handler** (FUN_18000889c, a 0x4430xx ioctl): allocates
  `count × stride` where stride = 0x23 (35 B) or 0x12 (18 B, when ctx+0x294
  != 0); fills via the internal template read (thunk FUN_180040114 → the
  45 0e-style command); then checks each record's FIRST BYTE == 0x01
  (the record-valid marker) and reports 0xff if any slot is empty.
  Template count = ctx byte +0x2c4 (from the `0d 0a` get_enroll_num reply:
  byte[1] = 10 = slot count).
- **45 07** (FUN_18003d424): READ 1 byte — finger-detect poll; used in
  FUN_18003c5bc's loop (×100, Sleep 50 ms, success when the byte == 1).
- **86 01** (FUN_18003e73c): WRITE 8 bytes (dlen=8) — the trailer/marker
  write; called with `buffer + size - 0x88` (the last 8 bytes before a
  0x88-byte tail region).
- **86 03** (FUN_18003cd58): the GENERIC FLASH WRITE — cmd =
  `86 03 00 00 00 00 [addr24 @ cmd[6..9]] [len16 @ cmd[10..11]]` + payload;
  FUN_18003ec60 chunks it ≤1024 B with the address incrementing from 0.
- **FUN_18003c5bc** = the upload sequence: `86 01` (trailer) → `86 03`
  chunks (whole buffer) → `45 07` completion poll. The address base starts
  at 0 — consistent with the DFU (firmware-update) path ("DFU" UTF-16
  strings sit next to the second op-table instance).
- SetMode handler remainder: after applying the mode it sets ctx+0x5cc=1,
  calls thunk_FUN_18004027c(param_1[7], 4) conditionally, FUN_18000b480,
  then FUN_18000544c(param_1) → indirect dispatch.
- 0x44309c handler (FUN_180006708) references "DBVersion" strings and calls
  the registered handler with (param_1[1], 0x101, 0x2001b) — a database
  version query.

### Open questions
1. The 85 0b 32-byte payload content: raw identity, standard SHA-256 of it
   (K constants are standard in the binaries), or something derived from the
   enroll record — the record body stays zero in all tested variants.
2. Whether the engine writes the template features via the 86 03 chunked
   writer before the 85 0b binding (the caller chain of op slot 3 is hidden
   behind the framework's registered-callback dispatch, DAT_180064e80).
3. A Windows-side USB capture of a real enrollment would resolve the exact
   byte sequence conclusively.

## Addendum 5 — payload variants exhausted; current state

Tested as the `85 0b` 32-byte payload (each: clean ACK, record readback state
flips 0x09↔0x0b, metadata counters at [66..73] change, identity body [1..60]
stays zero, template area (45 0e) and count stay unchanged):
- raw uid32 ("FP1-20261017-2-E513F8FD-justin\0\0")
- sha256(uid32)
- raw device key read via `46 16` (32 B: `31 35 31 32 33 19 1a 80 43 31 ff …`
  — starts with ASCII "15123", "C1", 0xff padding)
- sha256(device key)

Also ruled out: the capture-reply byte[3]-as-type hypothesis — 20 consecutive
captures all returned `00 [a4-c7] 23 01 00` (byte[3] constant 0x01; the reply
shape is `[status][u16 score ~0x23a4-0x23c7][01][00]`), no completion signal.

Device key read (`46 16`, dlen 32, READ): works, stable across sessions.

### Remaining unknowns
1. The exact `85 0b` payload the Windows engine sends. The caller chain of
   op-table slot 3 is hidden behind the CFG guard-dispatch indirection — the
   decompiled C and the byte-level disassembly both lose the vtable slot.
2. Whether a template-record upload (the `86 01` + `86 03` chunked writer,
   FUN_18003c5bc) precedes the store during Windows enrollment, and at which
   flash address. **Caution**: that writer is also the DFU path (address base
   0) — blind flash writes risk bricking the dongle.

### Recommended next steps
1. **Windows-side USB capture of a real enrollment** (USBPcap + Wireshark, or
   the usbmon-equivalent) — resolves the byte-for-byte sequence conclusively,
   including the store payload. Highest value, needs a Windows boot.
2. Static RE of the adapter-side engine functions that call
   EngineAdapterCommitEnrollment's successors (the 86-class writers) with
   template-shaped buffers.
3. File the upstream issue (draft ready) — the protocol documentation alone
   is a substantial contribution.

## Addendum 6 — sensor-adapter vendor slots + final state

The sensor-adapter interface vendor slots (vtable 0x1801152b0, slots [19]-[27]
= FUN_18000dc20..FUN_18000e160) are thin logging stubs, NOT the store op:
- slot [19] FUN_18000dc20 = sensor-adapter SetMode: validates the mode byte
  against {4, 10, 18} (bitmask 0x40410, max 0x12) — note: different mode set
  from the Wbdi SetMode ioctl ({1,2,4,16,0x80}).
- slots [20], [21] = no-op trace stubs (return 0).

Confirmed: the store (85 0b) payload = the raw 32-byte device key read at
driver init via `46 16` (ctx+0x435; the SHA-256 at ctx+0x455 is only logged).
The key was tested as the store payload — ACK but no persistence.

Exhaustive summary of the store-payload candidates tried: raw uid32,
sha256(uid32), raw device key, sha256(device key). All ACK, none persist.
Template area reads (45 0e, dlen 350/420/700/2048) and the count (45 0d →
`0d 0a`) are unchanged after every attempt.

### Bottom line
The remaining unknown is the engine-side state (or additional commands)
between the 8 capture results and the 85 0b store that makes the sensor
accept it. Most plausible resolutions, in order of value:
1. A Windows-side USB capture of a real enrollment (byte-for-byte truth).
2. Careful RE of the adapter-side engine enrollment state machine
   (EngineAdapterCreateEnrollment / UpdateEnrollment / CommitEnrollment and
   the pipeline glue) — the vendor vtable indirection hides these behind
   CFG-dispatch thunks.
3. Kensington developer support.

## Addendum 7 — MODE 4: the writable-database selector

The adapter-side SetMode-completion handler (FUN_18000ceb0) gates the ioctl
0x443030 (0-in/1-byte-out, dispatched through the generic framework callback
→ op-table) on **mode == 4**: `if (*(char *)(ctx + 8) != '\x04') goto skip;`
before sending 0x443030 and clearing the state counter at ctx+0xc.

- The sensor-adapter SetMode (FUN_18000dc20, slot [19]) validates modes
  {4, 10, 18} (bitmask 0x40410, max 0x12).
- The Wbdi SetMode ioctl (0x440014) validates {1, 2, 4, 16, 0x80} — mode 4 is
  in both sets.
- Windows flow: SetMode(4) → async 0x440014 completes → FUN_18000ceb0 →
  mode==4 → ioctl 0x443030 → generic dispatch → op-table slot → the store.
  So mode 4 selects the writable database context, and the store fires as
  part of the mode-4 completion processing.
- All our earlier store attempts ran in modes 0/1 — contexts where the store
  ACKs but discards.
- Revised test sequence (armed, background): captures in mode 1 →
  `05 13 04 [flag]` → `85 0b` + the 32-byte device key (`46 16` read:
  3135313233191a804331ffffffffffffc801ffff…) → `45 0c` readback → DB checks
  in modes 4/1/0.

## Addendum 8 — write-family verdict (post-reboot rerun)

After a /tmp wipe (system cleanup) everything was restored from ~/verimark
(the migration saved the session). Fresh venv, fresh commit_test.py with the
full latest flow. Run results (8/8 captures in mode 1, flag=2):

- `85 0a` + 32B payload (param 0): **heartbeat** (invalid in this context)
- `85 0b` + 32B payload: **clean ACK** — for BOTH raw-key and sha256(key)
- `85 21` + 32B payload (param 0): **heartbeat** (invalid)
- readbacks (45 0c, 74 B): body[1..65] all zeros in every case
- DB checks in modes 4/1/0: template area all zeros, count `0d 0a`

CONCLUSIONS:
1. `85 0b` is the only valid store command in this session state — confirming
   it as the real store/bind op (85 0a and 85 21 are rejected/no-op here).
2. The store ACKs with any 32-byte payload but never persists: raw key,
   sha256(key), raw uid32, sha256(uid32) — all identical behavior.
3. Mode 4 selects cleanly (post-captures) but does not unlock persistence by
   itself.
4. The sensor self-identifies as 0bda:5816 (40 06 read) — a stock upstream
   driver PID — but runs a Kensington/Atans firmware (0x41) with a different
   store protocol than the upstream driver assumes (85 21).

The missing piece is almost certainly the engine-side payload derivation from
the enroll record (the 8×9-byte capture results flow driver→engine via
\\.\Pipe\EnrollRecordPipe; the engine returns a 32-byte value that the sensor
verifies against its own accumulator). Without a Windows USB capture or
deeper C++-layer RE (FUN_180078e00 chain), the exact payload derivation
remains unknown.

## Addendum 9 — ★ LINUX ENROLLMENT AND IDENTIFICATION WORKING ★

The complete Windows-decoded enrollment flow was implemented in
~/verimark/enroll_test.py and executed successfully:

1. select_os `05 13 00 00`
2. template read `45 0e` dlen=520
3. delete target slot `05 0f [slot]`
4. dup-check: `05 05 02 02` → poll `45 06 02 02` → accept `45 08 02 00` (9B)
   → identify `45 22 00 02` (42B; byte0 0x09 = no-match-with-entries)
5. begin_pos `05 20 [slot]`
6. enroll captures ×8: `05 05 04 02` → poll `45 06 04 02` → accept
   `45 08 04 00` (9B)
7. check_dup `45 10` dlen=34
8. template read `45 0e` dlen=520
9. COMMIT: `85 21 [0xF5+slot] 00×8 dlen=49` + the 49-byte device-bound blob
   `01 05 00 00 00 00 00 05 15 00 00 00 f0f1e8e1 5c30c9d5 0de543af eb03
   00×9 fb44 9998 fcbc 3ce7 ad4b bcfc e73c ad4a`
10. Verify: slot stored with byte[2] = 0xF5+slot ✓, valid slots [0,1,2]
11. Identify with the new finger: byte0 = 0x00 = MATCH ✓

Identify reply semantics: byte0 = 0x00 match / 0x09, 0x0b no-match variants.
The commit blob is device-bound (identical for both Windows enrollments of
different fingers) — replayable. Record: [0]=0x01 valid, [1]=0x00,
[2]=0xF5+slot, [3]=0x01, [4..51]=48-byte body.

Remaining: integrate into the libfprint realtek driver (rework enroll/verify
to this protocol), then fprintd + PAM.

## Addendum 10 — driver integration status (fprintd)

WORKING:
- fprintd-enroll: 8/8 stages, enroll-completed (the reworked driver flow)
- The sensor protocol fully integrated: select_os 05 13 00 00, template
  geometry 52/520, capture params [4][2], commit 85 21 [0xF5+slot] dlen=49
  + the device blob, the verify params (purpose 2), the identify params
  (param[0]=0, param[1]=2), the match decode (byte0: 00=match,
  09/0b=no-match — the MATCH_FAIL range fix applied)

REMAINING (the final gaps):
1. fprintd-verify aborts at ListEnrolledFingers: "Failed to discover prints"
   — the driver's list vfunc parse (fp_print_from_data / parse_print_data)
   needs the 52-byte record layout: [0]=0x01 valid, [2]=0xF5+slot (the
   sub-factor), [3]=0x01, [4..51]=the body; the records have NO uid strings
   (the blob body) — the parse must not require them.
2. After the list fix: fprintd-verify → PAM wiring
   (/etc/pam.d/system-auth: `auth sufficient pam_fprintd.so`).
3. The enroll's host-side print save may need the same parse tolerance.

The sensor DB currently holds: slots 0/1 (the Windows fingers) + the
fprintd-enrolled finger (the last successful fprintd-enroll).

## Addendum 11 — ★ COMPLETE: fprintd verify + PAM working ★

End state: `fprintd-enroll`, `fprintd-list`, `fprintd-verify` (match and
no-match) all work, and PAM fingerprint auth works for pkexec/sudo (user
confirmed fingerprint-only auth). The DB was wiped at the user's request
(the Windows slots 0/1 were casualties of the 05 11 corruption below);
slot 0 now holds the fprintd-enrolled right index.

The four driver fixes (libfprint/drivers/realtek/realtek.c):

1. **fp_print_from_data** — the 52-byte records carry NO uid string:
   [0]=0x01 valid, [1]=0x00, [2]=0xF5+slot, [3]=0x01, [4..51]=body.
   It now builds fpi-data as `(y, ay=empty)`. CRITICAL: fprintd's storage
   sync compares host-stored vs device-listed prints with fp_print_equal
   (a byte compare of the fpi-data GVariant) and DELETES stored prints
   that don't match the device list ("unknown to device"). The saved
   enrollment print must be byte-identical to what the device list
   reports — hence fix 2.
2. **Enroll commit** — the print data is now `(0xF5+pos_index, empty uid)`
   instead of `(SUB_FINGER_01=0xFF, generated uid)`. This is what made
   verify work: the verify match reply is `00 [0xF5+slot] 01 <body…>`
   (status byte + the matched record minus its first two bytes), so
   `fp_print_from_data(buffer_in+1)` lands exactly on the sub-factor byte
   and fp_print_equal(stored, match) succeeds.
3. **05 11 update_template REMOVED from the verify SSM** — the stock
   driver sends 05 11 after every successful verify match; on this
   firmware it triggers a ~4 s flash operation + firmware reboot that
   CORRUPTED the sensor's matching state (verify-match at 19:47, then the
   same finger could only match the stale slot 0, dup-check flagged
   duplicates, template markers still read valid). Never send 05 11.
   The update-template state is now a no-op that completes the SSM.
4. **Match-status decode** — the identify reply's no-match codes are
   0x09/0x0b, which collide with the enum values TOO_SHORT(9)/
   MATCH_FAIL(11); the old retry range `[TOO_HIGH..MATCH_FAIL]` turned
   every no-match into an endless retry-scan loop. Only statuses
   1..8 are capture-quality retries now; 0x09/0x0b report no-match.

Also fixed: parse_print_data accepts uid-less prints (and any 0xF5..0xFE
sub-factor), and the delete path locates the slot by sub-factor byte
instead of the uid description string (delete via fprintd-delete verified
working — slot 4 was removed from the device).

Identify match reply (42 B), from a live capture:
  `00 f5 01 05 00 00 00 00 00 00 05 15 00 00 00 f0f1e8e1 …(record body)
   … 49 b2 01 00 9a 2b 00 00` — the trailing words look like counters/
  timestamps; the matched record starts at reply byte 1 (finger first).

No-match replies observed: 0x09 and 0x0b variants; quality statuses on
identify (e.g. 0x03 = too-left-style codes) also occur when placement is
poor and are treated as retries.

PAM: `auth sufficient pam_fprintd.so` added to /etc/pam.d/system-auth
(above the -pam_systemd_home line). Everything chains through system-auth
here: sudo, polkit-1, sddm (login), and the KDE lock screen
(/usr/lib/pam.d/kde → system-local-login → system-login → system-auth).
The systemd drop-in (LD_LIBRARY_PATH=/run/libfprint-custom +
BindReadOnlyPaths → the builddir) persists across reboots.
