#!/usr/bin/env bash
# Runs inside the privileged builder container started by create.sh.
# Partitions /vm/disk.img on first use, unpacks a verified stage3, then
# provisions the system through chroot.sh. Each step is skipped when already
# done, so a failed run can be resumed.
set -euo pipefail

img=/vm/disk.img
root=/mnt/gentoo
loop=

cleanup() {
    set +e
    if mountpoint -q "$root"; then
        umount -R "$root" || umount -Rl "$root"
    fi
    [[ -n $loop ]] && losetup -d "$loop"
    loop=
}
trap cleanup EXIT

if [[ ! -f $img ]]; then
    truncate -s "$VM_DISK_SIZE" "$img"
    sfdisk --quiet "$img" <<'EOF'
label: gpt
size=512MiB, type=uefi, name=esp
type=linux, name=root
EOF
    chown "$HOST_UID:$HOST_GID" "$img"
fi

loop=$(losetup -fP --show "$img")
for _ in 1 2 3 4 5; do [[ -b ${loop}p2 ]] && break; sleep 1; done
esp=${loop}p1
rootdev=${loop}p2
[[ -b $rootdev ]] || { echo "missing partition node $rootdev" >&2; exit 1; }

[[ -n $(blkid -p -s TYPE -o value "$rootdev") ]] || mkfs.ext4 -q -L shaode-root "$rootdev"
mkdir -p "$root"
mount "$rootdev" "$root"

if [[ ! -f $root/etc/gentoo-release ]]; then
    base=https://distfiles.gentoo.org/releases/amd64/autobuilds
    list=latest-stage3-amd64-desktop-openrc.txt
    cd /tmp
    gpg --quiet --import /usr/share/openpgp-keys/gentoo-release.asc
    wget -q "$base/$list"
    gpg --quiet --verify "$list"
    stage=$(grep -o '^[^ #]*\.tar\.xz' "$list" | head -1)
    echo "Fetching $stage"
    wget -q "$base/$stage" "$base/$stage.asc"
    gpg --quiet --verify "${stage##*/}.asc" "${stage##*/}"
    tar xpf "${stage##*/}" --xattrs-include='*.*' --numeric-owner -C "$root"
    rm -f "${stage##*/}"*
fi

mount --types proc /proc "$root/proc"
mount --rbind /sys "$root/sys" && mount --make-rslave "$root/sys"
mount --rbind /dev "$root/dev" && mount --make-rslave "$root/dev"
cp -L /etc/resolv.conf "$root/etc/resolv.conf"

install -d "$root/root/vm-setup"
cp /scripts/chroot.sh /vm/authorized_keys "$root/root/vm-setup/"

chroot "$root" env -i HOME=/root TERM="${TERM:-dumb}" \
    PATH=/usr/sbin:/usr/bin:/sbin:/bin \
    VM_USER="$VM_USER" VM_TIMEZONE="$VM_TIMEZONE" JOBS="$JOBS" \
    ESP_DEV="$esp" ROOT_DEV="$rootdev" \
    bash /root/vm-setup/chroot.sh

cleanup
echo "Disk image ready: $img"
