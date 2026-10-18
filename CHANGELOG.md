# Changelog

All notable changes to the verimark-kit are documented here.
Format loosely based on [Keep a Changelog](https://keepachangelog.com/);
versioning follows [SemVer](https://semver.org/).

## v1.1.2 — pacman repository automation (2026-10-18)

Arch users can now install straight from `pacman`. Every release
automatically builds the Arch package and publishes a self-hosted pacman
repository, so no manual `pacman -U` step is needed.

### Added
- Self-hosted pacman repository, published to the `pacman-repo` branch of
  the GitHub mirror on every tag. Install with:

        [verimark]
        SigLevel = Optional TrustAll
        Server = https://raw.githubusercontent.com/jedmund/verimark-kit/pacman-repo/$repo/$arch

  then `sudo pacman -Syu verimark-kit`.
- CI job `pkg:arch` — builds the `verimark-kit` package from the tagged
  source (same recipe as `aur/PKGBUILD`).
- CI job `publish:pacman-repo` — runs `repo-add` and republishes the
  repository database and packages.

### Fixed
- `release:github` now uses plain curl against the GitHub API (the `gh`
  CLI hit TLS verification problems on the CI runner and isn't packaged
  for Alpine 3.20). Release creation/upload is idempotent — re-running a
  pipeline updates the existing release instead of failing.

## v1.1.1 — Arch packaging (2026-10-18)

### Added
- `aur/PKGBUILD` — a source-based Arch package recipe. Builds the
  realtek driver from the tagged source, installs to `/usr/lib/verimark`
  (no conflicts with the stock `libfprint` package), ships an fprintd
  vendor drop-in at `/usr/lib/systemd/system/fprintd.service.d/` (no
  `/etc` changes), and the `verimark-pam` helper for wiring/unwiring PAM.
- `packaging/verimark-pam` — idempotent helper:
  `sudo verimark-pam` enables, `sudo verimark-pam remove` disables.
- Package `post_install` message with the three-step setup.

### Notes
- AUR submission is pending (AUR account registration is currently
  closed); the PKGBUILD is in the repo and used by the pacman
  repository CI from v1.1.2 on.

## v1.1.0 — multi-distro support (2026-10-18)

The kit now works out of the box on Ubuntu/Debian in addition to Arch,
with per-distro builds produced automatically by CI.

### Added
- Ubuntu 22.04 CI build of the library (`prebuilt/ubuntu-22.04/`);
  runs on Ubuntu 22.04+, Debian 12+, Mint, Pop!_OS.
- `install.sh` now detects the distro (`/etc/os-release`) and picks the
  matching prebuilt library and PAM file — `/etc/pam.d/system-auth` on
  Arch, `/etc/pam.d/common-auth` on Debian/Ubuntu.
- Full patched libfprint source tree vendored as `libfprint/`
  (see `libfprint/KIT-NOTES.md` for deviations); CI builds it for both
  distros.
- Release tarballs now ship both prebuilt libraries plus the installer
  and documentation.
- Checks in `install.sh` for missing `fprintd`/`libpam-fprintd`, with
  the exact install command for each distro.

### Changed
- CI builds only the `realtek` driver (`-Ddrivers=realtek`), removing
  the openssl/pixman/udev/introspection dependencies of unrelated
  drivers and speeding up builds (~9 s Arch, ~48 s Ubuntu).

## v1.0.0 — initial release (2026-10-17)

First public release of the Kensington VeriMark Desktop 2.0
(047d:8228, Realtek RTS58xx / Atans firmware) fingerprint driver.

The stock libfprint has **no driver at all** for this device; this one
was written from scratch by reverse engineering the Windows driver
(Full protocol and captures: `docs/FINDINGS.md`).

### Features
- Enrollment (8-stage flow with device-side duplicate check),
  listing, deletion, and verify (match/no-match) through fprintd.
- Fingerprint authentication via PAM: sudo, polkit, display-manager
  login, and lock screen (verified: `sudo verimark-pam`, enroll,
  `fprintd-verify` match + no-match).
- Survives reboots; sensor state persists on-device.

### Known issues
- Windows Hello enrollments are stored in the same on-device database
  but are not visible to fprintd's host storage sync.
- Never send the `05 11` (update_template) command to this firmware —
  it corrupts the sensor database (the stock libfprint realtek driver
  would; this driver disables it). See README "Known pitfalls".
