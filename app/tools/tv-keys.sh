#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Presses remote keys on a rooted TV (ares device $ARES_DEVICE, default "tv"), for testing the
# player without anyone holding the remote. The app receives them as real remote keys.
#   tools/tv-keys.sh KEY [KEY...]
# KEY: up down left right ok back play pause playpause stop ff rew red green yellow blue,
#      a Linux key code, or sleep:<seconds>.
# Example: tools/tv-keys.sh ok sleep:2 right right   (pause, then skip twice)
#          tools/tv-keys.sh hold-ok:1                  (OK held for a second)
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"
dev="${ARES_DEVICE:-tv}"
[[ $# -ge 1 ]] || { sed -n '3,8p' "$0"; exit 2; }

codes=()
for k in "$@"; do
    case "$k" in
        up) codes+=(103) ;; down) codes+=(108) ;; left) codes+=(105) ;; right) codes+=(106) ;;
        ok) codes+=(28) ;; back) codes+=(412) ;;
        play) codes+=(207) ;; pause) codes+=(119) ;; playpause) codes+=(164) ;; stop) codes+=(128) ;;
        ff) codes+=(208) ;; rew) codes+=(168) ;;
        red) codes+=(398) ;; green) codes+=(399) ;; yellow) codes+=(400) ;; blue) codes+=(401) ;;
        sleep:*|[0-9]*) codes+=("$k") ;;
        hold-ok:*) codes+=("hold:28:${k#hold-ok:}") ;;
        *) echo "unknown key: $k" >&2; exit 2 ;;
    esac
done

timeout 60 ares-push -d "$dev" tv-keys.py /tmp/wnk-tv-keys.py >/dev/null
timeout 300 ares-shell -d "$dev" --no-pty -r "python /tmp/wnk-tv-keys.py ${codes[*]}"
