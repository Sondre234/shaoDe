# Gentoo test VM

Scripts for a disposable Gentoo (OpenRC + elogind) QEMU/KVM guest, used to
build shaoDe against Gentoo's packages and to exercise the standalone
`--session` DRM/libinput backend on a virtio GPU with virgl acceleration.

Host requirements: KVM, `qemu-system-x86_64`, OVMF (edk2) firmware, rsync,
ssh, and Docker access (membership in the `docker` group). The windowed mode
needs QEMU's virgl GPU devices; Arch packages them as separate modules, and
the `-gl` ones need their base modules too:
`qemu-hw-display-virtio-{gpu,vga,gpu-pci}{,-gl}`. No sudo is needed:
the disk image is partitioned and provisioned inside a privileged
`gentoo/stage3` container using loop devices.

```sh
tools/gentoo-vm/create.sh           # build ~/vms/shaode-gentoo/disk.img
tools/gentoo-vm/run.sh              # boot with a window (virgl)
tools/gentoo-vm/run.sh --headless   # boot without a window (plain VGA)
tools/gentoo-vm/ssh.sh              # shell in the guest
tools/gentoo-vm/sync-build.sh       # copy the tree, build, and run ctest
```

`create.sh` downloads and verifies the latest `stage3-amd64-desktop-openrc`,
uses the official x86-64-v3 binary package host where possible, and compiles
the rest (for example wlroots 0.20, qtwayland, and mesa with
`VIDEO_CARDS=virgl`) on the host's CPUs. Rerunning it on an existing image
resumes or updates the provisioning; delete `disk.img` to start over. The
package list and system settings live in `chroot.sh`.

The guest boots through GRUB on an EFI system partition and uses
`gentoo-kernel-bin`, so `emerge -u` inside the guest updates the kernel
normally. The guest user matches your host user name; both it and root have
the password `shaode`. SSH is forwarded to `127.0.0.1:2222` and accepts the
generated `~/vms/shaode-gentoo/id_ed25519` key and your own public keys. The
serial console is logged to `~/vms/shaode-gentoo/serial.log`.

To test the standalone session, log in on the VM window's text console and run:

```sh
cd ~/shaoDe && cmake --install build
dbus-run-session -- ~/.local/bin/shaode --session
```

Settings can be overridden with `SHAODE_VM_DIR`, `SHAODE_VM_USER`,
`SHAODE_VM_SSH_PORT`, `SHAODE_VM_DISK_SIZE`, `SHAODE_VM_MEMORY`,
`SHAODE_VM_CPUS`, and `SHAODE_VM_DISPLAY` (a QEMU `-display` value, default
`gtk,gl=on`; try `sdl,gl=on` if GTK fails). The password and passwordless sudo
make this a test machine only; don't expose its SSH port.
