# Kensington VeriMark (047d:8228) fingerprint driver — install kit

Everything needed to get the Kensington VeriMark Desktop 2.0 (Kensington
047d:8228, Realtek RTS58xx / Atans firmware) working on Linux with
libfprint 1.94.x, fprintd, and PAM (sudo / polkit / SDDM login / KDE lock
screen). Developed and tested on Arch Linux, October 2026.

## What's in the kit

| Path | What it is |
|---|---|
| `prebuilt/libfprint-2.so.2.0.0` | Ready-built libfprint including the working realtek driver (the easy path) |
| `src/realtek.c`, `src/realtek.h` | The patched driver sources, if you want to rebuild against your own libfprint tree |
| `systemd/override.conf` | fprintd systemd drop-in that makes fprintd load the custom library |
| `pam/` | The PAM line to add (install.sh does it for you) |
| `docs/FINDINGS.md` | Full reverse-engineering notes (protocol, addenda 1–11) |
| `docs/upstream-issue-draft.md` | Draft issue for libfprint upstream |
| `install.sh` | One-shot installer (root) |

## Requirements

- Arch Linux (or any distro with stock `libfprint` + `fprintd` packages)
- The device: `lsusb` should show `047d:8228` (Kensington VeriMark)
- `fprintd` installed (`pacman -S fprintd`)

## Install (the easy path)

```
cd ~/Downloads/verimark-kit
sudo ./install.sh
```

This:
1. copies the prebuilt library to `/opt/verimark/libfprint`
2. installs the systemd drop-in (`/etc/systemd/system/fprintd.service.d/override.conf`)
   so fprintd loads it via `LD_LIBRARY_PATH`
3. adds `auth sufficient pam_fprintd.so` to `/etc/pam.d/system-auth`
4. restarts fprintd

## Enroll and test

```
fprintd-enroll -f right-index-finger $USER
```

Press and lift the finger ~8 times, then **one more press** at the end
(the duplicate check needs a final scan). When it prints
`enroll-completed`, test:

```
fprintd-verify $USER          # match with the enrolled finger,
                              # no-match with any other
fprintd-list $USER            # the finger must still be listed!
```

Then fingerprint works for `sudo`, `pkexec`/polkit prompts, the SDDM
login screen, and the KDE lock screen — all of them chain through
`/etc/pam.d/system-auth` on Arch.

## Rebuilding from source (optional)

`src/realtek.c` / `src/realtek.h` drop into an existing libfprint source
tree at `libfprint/drivers/realtek/` (the tree in use lives at
`~/verimark/libfprint`, built with `meson compile -C builddir`). If you
rebuild, install the resulting `libfprint-2.so.2.0.0` the same way
(see `install.sh`) or adjust the `BindReadOnlyPaths` in the drop-in.

The current machine (the one this kit was created on) still loads its
build directly from `~/verimark/libfprint/builddir` via its existing
drop-in — running this kit's `install.sh` here would switch it to
`/opt/verimark/libfprint`, which is fine.

## Known pitfalls (do not re-introduce these!)

1. **The saved print must byte-match the device record.** fprintd's
   storage sync compares host-stored prints to device-listed prints with
   `fp_print_equal` (a byte compare of the `fpi-data` GVariant) and
   silently deletes stored prints that don't match ("unknown to
   device"). The device records are `[01][00][F5+slot][01]<48-byte body>`
   — no uid string — so the enrollment print data must be stored as
   `(0xF5+slot, empty uid)`.
2. **Never send `05 11` (update_template) to this firmware.** It
   triggers a ~4 s flash operation + firmware reboot and corrupts the
   sensor's matching state. The stock driver sends it after every
   successful verify; the patched driver makes that state a no-op.
3. **Identify no-match codes are 0x09/0x0b**, which collide with the
   driver's retry-status enum (`TOO_SHORT`=9, `MATCH_FAIL`=11). Only
   statuses 1–8 are capture-quality retries.
4. If `fprintd-verify` works once and then stops matching forever, the
   sensor DB was corrupted (see 2) — erase it with `05 0f param=0xff`
   (clear storage) and re-enroll.

Full protocol documentation: `docs/FINDINGS.md` (esp. Addendum 9–11).
