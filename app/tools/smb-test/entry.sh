#!/bin/sh
# The share's user, with the password from the environment.
set -e
adduser -D -H -s /sbin/nologin vplayer 2>/dev/null || true
printf '%s\n%s\n' "$SMB_PASSWORD" "$SMB_PASSWORD" | smbpasswd -a -s vplayer
exec smbd --foreground --no-process-group --debug-stdout
