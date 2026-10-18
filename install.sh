#!/bin/bash
# Kensington VeriMark (047d:8228) fingerprint driver kit — installer.
#
# Installs the prebuilt libfprint (with the working realtek driver) to
# /opt/verimark/libfprint, adds the fprintd systemd drop-in, and wires
# PAM. Run with root privileges:  sudo ./install.sh
set -euo pipefail

KIT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LIB_DIR="/opt/verimark/libfprint"
DROPIN_DIR="/etc/systemd/system/fprintd.service.d"
PAM_FILE="/etc/pam.d/system-auth"
PAM_LINE="auth       sufficient                  pam_fprintd.so"

[[ $EUID -eq 0 ]] || { echo "Run as root: sudo ./install.sh"; exit 1; }

echo "==> Installing driver library to $LIB_DIR"
install -d -m 755 "$LIB_DIR"
install -m 755 "$KIT_DIR/prebuilt/libfprint-2.so.2.0.0" "$LIB_DIR/"
ln -sfn libfprint-2.so.2.0.0 "$LIB_DIR/libfprint-2.so.2"
ln -sfn libfprint-2.so.2.0.0 "$LIB_DIR/libfprint-2.so"

echo "==> Installing fprintd systemd drop-in"
install -d -m 755 "$DROPIN_DIR"
install -m 644 "$KIT_DIR/systemd/override.conf" "$DROPIN_DIR/override.conf"

echo "==> Checking PAM ($PAM_FILE)"
if grep -q "pam_fprintd.so" "$PAM_FILE"; then
    echo "    pam_fprintd already present, skipping"
else
    if ! grep -q "pam_faillock.so      preauth" "$PAM_FILE"; then
        echo "    WARNING: expected 'pam_faillock.so preauth' line not found;"
        echo "    insert '$PAM_LINE' manually at the top of the auth stack."
    else
        sed -i "/pam_faillock.so      preauth/a $PAM_LINE" "$PAM_FILE"
        echo "    added: $PAM_LINE"
    fi
fi

echo "==> Reloading systemd and restarting fprintd"
systemctl daemon-reload
systemctl restart fprintd

echo "==> Done. Enroll a finger with:"
echo "      fprintd-enroll -f right-index-finger \$USER"
echo "    then test with:"
echo "      fprintd-verify \$USER"
echo "    and check that 'fprintd-list \$USER' still shows the finger"
echo "    afterwards (if it disappears, the print-data format is wrong)."
