# Kensington VeriMark (047d:8228) fingerprint driver — install kit

Everything needed to get the Kensington VeriMark Desktop 2.0 (Kensington
047d:8228, Realtek RTS58xx / Atans firmware) working on Linux with
libfprint 1.94.x, fprintd, and PAM (sudo / polkit / display-manager login
/ lock screen). Developed and tested on Arch Linux; CI also builds the
library on Ubuntu 22.04. October 2026.

## What's in the kit

| Path | What it is |
|---|---|
| `install.sh` | One-shot installer (root) — detects Arch vs Debian/Ubuntu, installs the matching prebuilt library, wires PAM |
| `prebuilt/arch/` | libfprint built on Arch (the proven, tested binary) |
| `prebuilt/ubuntu-22.04/` | libfprint built on Ubuntu 22.04 (CI; runs on Ubuntu 22.04+, Debian 12+, Mint, Pop!_OS) |
| `libfprint/` | The full patched libfprint source tree (see `libfprint/KIT-NOTES.md`) — rebuild anywhere |
| `systemd/override.conf` | fprintd systemd drop-in that makes fprintd load the custom library |
| `pam/` | Example PAM wiring for both distros (install.sh does it for you) |
| `docs/FINDINGS.md` | Full reverse-engineering notes (protocol, addenda 1–11) |
| `docs/upstream-issue-draft.md` | Draft issue for libfprint upstream |

The stock libfprint has no driver for this device at all — the driver is
a from-scratch port based on a Windows-driver reverse engineering
(`docs/FINDINGS.md`).

## Requirements

- Arch Linux, or Ubuntu 22.04+ / Debian 12+ (Linux Mint / Pop!_OS work too)
- The device: `lsusb` should show `047d:8228` (Kensington VeriMark)
- `fprintd` (plus `libpam-fprintd` on Ubuntu) — the installer tells you
  the exact command if they're missing

## Install (the easy path)

**Option A — download a release tarball** (Releases page → download
`verimark-kit-<tag>.tar.gz`, verify with the attached `SHA256SUMS`;
tarballs ship prebuilt libraries for both distro families):

```
tar xzf verimark-kit-<tag>.tar.gz
cd verimark-kit
sudo ./install.sh
```

**Option B — clone this repo** (prebuilt library currently included for
Arch only; on Ubuntu, use a release tarball or build from `libfprint/`):

```
git clone https://git.atelier.house/jedmund/verimark-kit.git
cd verimark-kit
sudo ./install.sh
```

**Option C — pacman repository (Arch only).** Every release
automatically builds the Arch package and updates a self-hosted pacman
repository. Add this to `/etc/pacman.conf`:

```
[verimark]
SigLevel = Optional TrustAll
Server = https://raw.githubusercontent.com/jedmund/verimark-kit/pacman-repo/$repo/$arch
```

then:

```
sudo pacman -Syu verimark-kit
```

(The repo is unsigned; that's what `SigLevel = Optional TrustAll` is
for — drop it if/when packages become signed. The package installs the
driver to `/usr/lib/verimark`, an fprintd vendor drop-in, and the
`verimark-pam` helper; it does not touch `/etc`.)

The installer:
1. detects your distro and picks the matching prebuilt library
2. installs it to `/opt/verimark/libfprint`
3. installs the systemd drop-in (`/etc/systemd/system/fprintd.service.d/override.conf`)
   so fprintd loads it via `LD_LIBRARY_PATH`
4. adds `auth sufficient pam_fprintd.so` to your PAM stack
   (`/etc/pam.d/system-auth` on Arch, `/etc/pam.d/common-auth` on
   Debian/Ubuntu)
5. restarts fprintd

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

Fingerprint auth then works for `sudo`, `pkexec`/polkit prompts, the
display-manager login screen, and the lock screen:

- **Arch**: everything chains through `/etc/pam.d/system-auth`
  (sudo, polkit-1, sddm, KDE lock screen via `/usr/lib/pam.d/kde`).
- **Ubuntu/Debian**: everything includes `/etc/pam.d/common-auth`
  (sudo, polkit, GDM login; GDM's own fingerprint stack is also fine).

You can also enroll from the desktop settings app (KDE System Settings →
Users → fingerprint, GNOME Settings → Users → Fingerprint Login) — it's
a GUI for the same fprintd API.

## Building from source

`libfprint/` is the full patched tree:

```
cd libfprint
meson setup builddir -Dtests=false -Dexamples=false
meson compile -C builddir
```

Then install the result the same way `install.sh` does (or adjust the
`BindReadOnlyPaths` in the drop-in). CI does exactly this for both
distros — see `.gitlab-ci.yml`. The driver of interest is
`libfprint/libfprint/drivers/realtek/realtek.{c,h}`.

Note: the machine this kit was developed on still loads its build
directly from `~/verimark/libfprint/builddir` via its own drop-in —
running this kit's `install.sh` there switches it to
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
