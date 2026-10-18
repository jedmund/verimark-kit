# [realtek] Kensington VeriMark Desktop 2.0 (047d:8228) — detection works, enrollment capture works, template commit does not persist

## Device

- Kensington VeriMark Desktop 2.0 (K64741WW), USB `047d:8228`
- lsusb: `047d:8228 Kensington fingerprint device`, iSerial `201801010001`, bcdDevice 0.13
- Single vendor-class interface (subclass 2); EP 0x01 bulk OUT, EP 0x82 bulk IN, EP 0x83 int IN
- Windows driver: `kensingtonmocwbdi.inf` → AtansMocWbdi/Adapter/Util.dll (Realtek `rtswbdi`,
  same "Realtek USB2.0 Finger Print Bridge" stack as `0bda:5813/5816`, `2541:fa03` Microctopus)

## What works with the upstream `realtek` driver + these patches

1. ID-table entry `{0x047d, 0x8228}` (EP layout matches EP_OUT=0x01/EP_IN=0x82 exactly).
2. Raised timeouts (CMD 5s / DATA 15s / STATUS 8s) — the device re-arms its bulk
   endpoints slowly after resets.
3. Draining the unsolicited 5-byte frame (`01 f7 ff ff ff`) pushed on EP 0x82 after
   every USB reset — otherwise it is consumed as the first command's status and
   desynchronizes the session.
4. With those, the full enrollment capture pipeline works: `select_os`, `begin_pos`,
   8× capture/accept_sample with quality feedback (TOO_HIGH/TOO_LOW/...),
   `check_duplicate` (0x0b = MATCH_FAIL), storage enumeration (10 slots,
   delete/re-add verified).

## What does not work

The enrollment commit never persists a template. The stock `85 21 param=0xFF
dlen=32 + 32 B uid payload` is ACKed but the on-device database (read back with
`45 0e`, both `select_os` modes) stays all-zero; `45 0d` count reply stays `0d 0a`.

Exhaustively tried, in real capture sessions:
- `85 21` with dlen 32/28/35(record-shaped), param 0xFF/pos — no storage
- `85 21` dlen=0, `85 22`, `85 20` — ACK only, no storage
- all `85`-class subs 0x00–0x40 payload-less (in-session sweep) — all ACK
- all `45`-class subs 0x00–0x40 payload-less — no new behavior
- all `05`-class subs 0x00–0x40 payload-less — three previously unknown valid
  opcodes found: `05 12`, `05 15`, `05 16` (plus known 05 05/07/0f/11/13/20)
- `05 12` + 32 B payload → device streams zero-filled data (2 KiB observed) then
  wedges; it is a data-transfer command, not the commit
- `05 11` (upstream's update_template) — triggers a ~4 s flash operation and a
  firmware reboot and ERASES stored data; must never be sent during enrollment
- `select_os` mode 0 vs 1 (the mode selects on-device template databases; mode 1
  behaves like the SDCP-protected Windows Hello database, mode 0 answers
  database queries properly) — commit does not persist in either

## Key protocol findings (useful beyond this device)

- Reply `01 f7 ff ff ff` on EP 0x82 is the sensor's spontaneous **idle heartbeat**,
  pushed whenever it finishes processing (also right after USB resets) — it is not
  an error code. Identical behaviour confirmed on sibling `2541:fa03`.
- During an active session the real ACK is `00 00 00 00 00` or `00 f7 ff ff ff`
  (status 00 + device marker `f7ffffff`).
- `select_os` (`05 13` param) selects the template database: mode 1 = Windows Hello
  SDCP database (unauthenticated database operations silently discarded), mode 0 =
  open database (check_duplicate answers properly). Consistent with published SDCP
  research (separate per-driver databases selected by an unauthenticated init packet).
- Device-info control read (bRequest 0x07 wValue 0x000D) replies
  `39 00 00 00 ff ff ff ff` (0x39 is *not* the template record length; records are
  35 B as in the stock driver).

## Hypotheses for the missing commit

- The Atans firmware gates database writes behind a session/handshake established
  by the Windows driver beyond `select_os` (candidate: the ioctl `0x440004` payload
  the sensor adapter builds in `SensorAdapterSetMode`, translated to USB inside
  AtansMocWbdi.dll), or
- the device requires the SDCP/ESS secure channel for database writes (device is
  marketed as ESS/SDCP-certified; stock `0bda:5813` ships with SDCP disabled).

Full notes, usbmon captures and probe tooling: available on request.
Driver patches against libfprint 1.94.100 `drivers/realtek/`: available on request.

## Postscript (updated after full bring-up)

The issues above were solved on the Kensington 047d:8228 (Atans firmware).
Corrections to this draft:

- Template records are **52 B** (not 35 B as in the stock driver): `[0]=0x01`
  valid marker, `[1]=0x00`, `[2]=0xF5+slot` sub-factor, `[3]=0x01`, `[4..51]`
  body. No host uid string is stored on the device. Geometry: 52/520.
- The working commit is `85 21 [0xF5+slot] dlen=49` + the 49-byte device-bound
  blob (identical for every enrollment; replayable — decoded from the Windows
  capture). Enroll flow: select_os 0 → get_enroll_num → delete target slot →
  dup-check (05 05 02 02 / 45 06 / 45 08 / 45 22) → begin_pos 05 20 [slot] →
  8 captures (params [4][2]) → check_dup 45 10 dlen=34 → template read →
  commit. Verify/identify: params [0][2], purpose 2; match = reply byte0 0x00,
  no-match = 0x09/0x0b.
- **The stock driver's `05 11` update_template after a successful verify must
  NOT be sent to this firmware**: it triggers a ~4 s flash operation + firmware
  reboot and corrupts the stored templates/matching state.
- With those fixes (plus print data stored as `(0xF5+slot, empty uid)` so
  fprintd's fp_print_equal sync doesn't delete stored prints), enrollment,
  listing, verify (match + no-match) and PAM authentication all work.
