# Shared settings for the Gentoo test VM scripts. Sourced, not executed.

VM_DIR=${SHAODE_VM_DIR:-$HOME/vms/shaode-gentoo}
VM_USER=${SHAODE_VM_USER:-$USER}
VM_SSH_PORT=${SHAODE_VM_SSH_PORT:-2222}
VM_DISK_SIZE=${SHAODE_VM_DISK_SIZE:-64G}
VM_MEMORY=${SHAODE_VM_MEMORY:-8G}
VM_CPUS=${SHAODE_VM_CPUS:-8}
VM_BUILDER_IMAGE=gentoo/stage3:amd64-desktop-openrc

OVMF_CODE=/usr/share/edk2/x64/OVMF_CODE.4m.fd
OVMF_VARS_TEMPLATE=/usr/share/edk2/x64/OVMF_VARS.4m.fd

vm_ssh() {
    ssh -i "$VM_DIR/id_ed25519" -p "$VM_SSH_PORT" \
        -o UserKnownHostsFile="$VM_DIR/known_hosts" \
        -o StrictHostKeyChecking=accept-new \
        -o ConnectTimeout=5 \
        "$VM_USER@127.0.0.1" "$@"
}
