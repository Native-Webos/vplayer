#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# A test SMB share on this PC, for VPlayer on the TV (docs/TV-TEST-PLAN.md section 3):
#   tools/smb-test/run.sh start SHARE_DIR [PASSWORD] [ADDRESS]   share SHARE_DIR read-only as \\ADDRESS\media
#   tools/smb-test/run.sh stop
# User "vplayer"; the password is generated when not given and printed. ADDRESS: the PC's address
# the TV reaches (default: the one on the TV's network). Needs Docker; port 445 must be free.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
name=vplayer-smb-test
case "${1:-}" in
    start)
        dir="$(cd "${2:?usage: run.sh start SHARE_DIR [PASSWORD] [ADDRESS]}" && pwd)"
        pass="${3:-$(head -c 9 /dev/urandom | base64 | tr -d '/+=')}"
        addr="${4:-$(ip -4 route get 1.1.1.1 | awk '{for (i = 1; i < NF; i++) if ($i == "src") print $(i + 1)}')}"
        docker build -q -t "$name" "$here" >/dev/null
        docker rm -f "$name" >/dev/null 2>&1 || true
        docker run -d --name "$name" -p "$addr:445:445" -v "$dir:/share:ro" -e SMB_PASSWORD="$pass" "$name" >/dev/null
        echo "share:    smb://$addr/media   (\\\\$addr\\media)"
        echo "user:     vplayer"
        echo "password: $pass"
        echo "stop:     $0 stop"
        ;;
    stop) docker rm -f "$name" >/dev/null && echo "stopped" ;;
    *) echo "usage: $0 start SHARE_DIR [PASSWORD] [ADDRESS] | stop" >&2; exit 2 ;;
esac
