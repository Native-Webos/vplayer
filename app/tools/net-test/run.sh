#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# DLNA, FTP and SFTP test servers on this PC, for VPlayer on the TV (docs/TV-TEST-PLAN.md section 3):
#   tools/net-test/run.sh start SHARE_DIR [PASSWORD]   DLNA "VPlayer test" (found by discovery),
#                                                      ftp://ADDRESS:2121/, sftp://ADDRESS:2222/
#   tools/net-test/run.sh stop
# User "vplayer" for FTP and SFTP; the password is generated when not given. The container uses the
# host's network (DLNA announces itself by multicast), so the ports must be free.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
name=vplayer-net-test
case "${1:-}" in
    start)
        dir="$(cd "${2:?usage: run.sh start SHARE_DIR [PASSWORD]}" && pwd)"
        pass="${3:-$(head -c 9 /dev/urandom | base64 | tr -d '/+=')}"
        addr="$(ip -4 route get 1.1.1.1 | awk '{for (i = 1; i < NF; i++) if ($i == "src") print $(i + 1)}')"
        docker build -q -t "$name" "$here" >/dev/null
        docker rm -f "$name" >/dev/null 2>&1 || true
        docker run -d --name "$name" --network host -v "$dir:/share:ro" -e NET_PASSWORD="$pass" -e NET_ADDRESS="$addr" \
            "$name" >/dev/null
        echo "DLNA:     \"VPlayer test\" (http://$addr:8200/)"
        echo "FTP:      ftp://$addr:2121/"
        echo "SFTP:     sftp://$addr:2222/"
        echo "user:     vplayer"
        echo "password: $pass"
        echo "stop:     $0 stop"
        ;;
    stop) docker rm -f "$name" >/dev/null && echo "stopped" ;;
    *) echo "usage: $0 start SHARE_DIR [PASSWORD] | stop" >&2; exit 2 ;;
esac
