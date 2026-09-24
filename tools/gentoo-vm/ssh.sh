#!/usr/bin/env bash
# SSH into the running Gentoo test VM; arguments run as a remote command.
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
. "$here/common.sh"

vm_ssh "$@"
