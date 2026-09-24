#!/usr/bin/env bash
# Build or update the Gentoo test VM disk image.
#
# Runs as your user; the privileged work happens in a Docker container
# (membership in the docker group is required). Rerunning it on an existing
# image skips partitioning and repeats provisioning, which picks up changes to
# the package list and configuration.
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
. "$here/common.sh"

mkdir -p "$VM_DIR"
# Disable copy-on-write for the disk image on btrfs; harmless elsewhere.
chattr +C "$VM_DIR" 2>/dev/null || true
[[ -f $VM_DIR/id_ed25519 ]] ||
    ssh-keygen -q -t ed25519 -N '' -C shaode-vm -f "$VM_DIR/id_ed25519"
[[ -f $VM_DIR/OVMF_VARS.4m.fd ]] || cp "$OVMF_VARS_TEMPLATE" "$VM_DIR/"

keys=$(cat "$VM_DIR/id_ed25519.pub" ~/.ssh/id_*.pub 2>/dev/null || true)
printf '%s\n' "$keys" >"$VM_DIR/authorized_keys"

# /dev is shared so loop partition nodes created after start are visible.
exec docker run --rm --privileged -v /dev:/dev \
    -v "$here:/scripts:ro" -v "$VM_DIR:/vm" \
    -e VM_USER="$VM_USER" -e VM_DISK_SIZE="$VM_DISK_SIZE" \
    -e VM_TIMEZONE="$(readlink /etc/localtime | sed 's|.*/zoneinfo/||')" \
    -e VM_KEYMAP="$(localectl status 2>/dev/null | sed -n 's/.*VC Keymap: //p')" \
    -e JOBS="$(nproc)" -e HOST_UID="$(id -u)" -e HOST_GID="$(id -g)" \
    "$VM_BUILDER_IMAGE" bash /scripts/builder.sh
