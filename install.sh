#!/bin/bash
# Kensington VeriMark (047d:8228) fingerprint driver kit — installer.
#
# Works on Arch Linux and Debian/Ubuntu-family systems. Installs the
# prebuilt libfprint (with the working realtek driver) to
# /opt/verimark/libfprint, adds the fprintd systemd drop-in, and wires
# PAM. Run with root privileges:  sudo ./install.sh
set -euo pipefail

KIT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LIB_DIR="/opt/verimark/libfprint"
DROPIN_DIR="/etc/systemd/system/fprintd.service.d"
PAM_LINE="auth       sufficient                  pam_fprintd.so"

[[ $EUID -eq 0 ]] || { echo "Run as root: sudo ./install.sh"; exit 1; }

# ---------------------------------------------------------------- distro
# Detect the distro family and pick the matching prebuilt library and PAM
# file. The Arch build runs on Arch-family; the Ubuntu build (made on
# 22.04) runs on Ubuntu 22.04+, Debian 12+, Mint, Pop!_OS, etc.
DISTRO="unknown"
PAM_FILE=""
if [[ -r /etc/os-release ]]; then
    . /etc/os-release
    case "${ID:-}${ID_LIKE:-}" in
        *arch*|*manjaro*|*artix*)
            DISTRO="arch"          PAM_FILE="/etc/pam.d/system-auth" ;;
        *debian*|*ubuntu*)
            DISTRO="ubuntu-22.04"  PAM_FILE="/etc/pam.d/common-auth" ;;
    esac
fi
if [[ "$DISTRO" == "unknown" ]]; then
    echo "Could not detect the distro from /etc/os-release."
    echo "Arch family -> prebuilt/arch | Debian/Ubuntu family -> prebuilt/ubuntu-22.04"
    exit 1
fi
echo "==> Detected distro: $DISTRO (PAM file: $PAM_FILE)"

SO="$KIT_DIR/prebuilt/$DISTRO/libfprint-2.so.2.0.0"
if [[ ! -f "$SO" ]]; then
    echo "No prebuilt library for this distro in the kit (expected"
    echo "  prebuilt/$DISTRO/libfprint-2.so.2.0.0)."
    echo "Download a release tarball, which ships both builds:"
    echo "  https://git.atelier.house/jedmund/verimark-kit/-/releases"
    exit 1
fi

# ------------------------------------------------------------ fprintd
if ! command -v fprintd-enroll >/dev/null 2>&1; then
    case "$DISTRO" in
        arch)         echo "fprintd is not installed. Run:  sudo pacman -S fprintd";  ;;
        ubuntu-22.04) echo "fprintd is not installed. Run:  sudo apt install fprintd libpam-fprintd";  ;;
    esac
    exit 1
fi

# ------------------------------------------------------------- library
if ! ls /usr/lib/security/pam_fprintd.so /usr/lib/*/security/pam_fprintd.so >/dev/null 2>&1; then
    case "$DISTRO" in
        arch)         echo "The PAM module pam_fprintd.so is missing (it ships with the fprintd package on Arch).";  ;;
        ubuntu-22.04) echo "The PAM module pam_fprintd.so is missing. Run:  sudo apt install libpam-fprintd";  ;;
    esac
    exit 1
fi

echo "==> Installing driver library to $LIB_DIR"
install -d -m 755 "$LIB_DIR"
install -m 755 "$SO" "$LIB_DIR/"
ln -sfn libfprint-2.so.2.0.0 "$LIB_DIR/libfprint-2.so.2"
ln -sfn libfprint-2.so.2.0.0 "$LIB_DIR/libfprint-2.so"

# ------------------------------------------------------------- drop-in
echo "==> Installing fprintd systemd drop-in"
install -d -m 755 "$DROPIN_DIR"
install -m 644 "$KIT_DIR/systemd/override.conf" "$DROPIN_DIR/override.conf"

# ----------------------------------------------------------------- PAM
echo "==> Wiring PAM ($PAM_FILE)"
if grep -q "pam_fprintd.so" "$PAM_FILE"; then
    echo "    pam_fprintd already present, skipping"
elif [[ "$DISTRO" == "arch" ]]; then
    if grep -q "pam_faillock.so      preauth" "$PAM_FILE"; then
        sed -i "/pam_faillock.so      preauth/a $PAM_LINE" "$PAM_FILE"
    else
        sed -i "1a $PAM_LINE" "$PAM_FILE"
    fi
    echo "    added: $PAM_LINE"
else
    # Debian/Ubuntu: insert before the pam_unix line of the auth stack;
    # fingerprint succeeds -> sufficient -> done, otherwise password auth
    # continues as usual.
    if grep -Eq "^auth.*pam_unix\.so" "$PAM_FILE"; then
        sed -i "/^auth.*pam_unix\.so/i $PAM_LINE" "$PAM_FILE"
    else
        sed -i "1a $PAM_LINE" "$PAM_FILE"
    fi
    echo "    added: $PAM_LINE"
fi

# -------------------------------------------------------------- reload
echo "==> Reloading systemd and restarting fprintd"
systemctl daemon-reload
systemctl restart fprintd

echo "==> Done. Enroll a finger with:"
echo "      fprintd-enroll -f right-index-finger \$USER"
echo "    then test with:"
echo "      fprintd-verify \$USER"
echo "    and check that 'fprintd-list \$USER' still shows the finger"
echo "    afterwards (if it disappears, the print-data format is wrong)."
