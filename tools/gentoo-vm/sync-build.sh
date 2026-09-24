#!/usr/bin/env bash
# Copy the working tree into the running VM (~/shaoDe), then build and test
# it there. Build directories and editor state are not copied.
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
. "$here/common.sh"
repo=$(cd "$here/../.." && pwd)

rsync -a --delete \
    --exclude='/build*/' --exclude='/.cache/' --exclude='/.idea/' \
    --exclude='/compile_commands.json' \
    -e "ssh -i $VM_DIR/id_ed25519 -p $VM_SSH_PORT -o UserKnownHostsFile=$VM_DIR/known_hosts -o StrictHostKeyChecking=accept-new" \
    "$repo/" "$VM_USER@127.0.0.1:shaoDe/"

vm_ssh 'cd shaoDe &&
    cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
        -DCMAKE_INSTALL_PREFIX="$HOME/.local" &&
    cmake --build build &&
    ctest --test-dir build --output-on-failure'
