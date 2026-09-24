#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Boot the Gentoo test VM.
#   run.sh             window with virgl 3D acceleration (virtio-gpu)
#   run.sh --headless  no window, standard VGA; use ssh.sh
# SSH is forwarded to 127.0.0.1:$VM_SSH_PORT; the serial console is logged
# to $VM_DIR/serial.log.
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
. "$here/common.sh"

display=(-display "${SHAODE_VM_DISPLAY:-gtk,gl=on}" -device virtio-vga-gl)
if [[ ${1:-} == --headless ]]; then
    display=(-display none -device VGA)
    shift
elif ! qemu-system-x86_64 -device help | grep -q '"virtio-vga-gl"'; then
    echo "QEMU lacks virtio-vga-gl (or use --headless). On Arch install:" >&2
    echo "  qemu-hw-display-virtio-{gpu,vga,gpu-pci}{,-gl}" >&2
    exit 1
fi

[[ -f $VM_DIR/disk.img ]] || { echo "No disk image; run create.sh first" >&2; exit 1; }

exec qemu-system-x86_64 \
    -name shaode-gentoo -enable-kvm -machine q35 -cpu host \
    -smp "$VM_CPUS" -m "$VM_MEMORY" \
    -drive if=pflash,format=raw,readonly=on,file="$OVMF_CODE" \
    -drive if=pflash,format=raw,file="$VM_DIR/OVMF_VARS.4m.fd" \
    -drive file="$VM_DIR/disk.img",format=raw,if=virtio,discard=unmap \
    "${display[@]}" \
    -device virtio-keyboard-pci -device virtio-tablet-pci \
    -nic user,model=virtio-net-pci,hostfwd=tcp:127.0.0.1:"$VM_SSH_PORT"-:22 \
    -serial file:"$VM_DIR/serial.log" \
    "$@"
