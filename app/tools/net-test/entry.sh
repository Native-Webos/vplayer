#!/bin/sh
# One user for FTP and SFTP; DLNA needs none. Everything reads /share (mounted read-only).
set -e
adduser -D -h /share -s /bin/sh vplayer 2>/dev/null || true
echo "vplayer:$NET_PASSWORD" | chpasswd
# SFTP on 2222 (password sign-in only, the share as home).
ssh-keygen -A >/dev/null
cat > /etc/ssh/sshd_config.d/test.conf <<CONF
Port 2222
PasswordAuthentication yes
KbdInteractiveAuthentication no
Subsystem sftp internal-sftp
CONF
/usr/sbin/sshd
# FTP on 2121, passive ports 30000-30009 on the given address.
cat > /etc/vsftpd/vsftpd.conf <<CONF
listen=YES
listen_port=2121
anonymous_enable=NO
local_enable=YES
write_enable=NO
chroot_local_user=YES
allow_writeable_chroot=YES
local_root=/share
pasv_enable=YES
pasv_min_port=30000
pasv_max_port=30009
pasv_address=$NET_ADDRESS
seccomp_sandbox=NO
background=YES
CONF
vsftpd /etc/vsftpd/vsftpd.conf
# DLNA on 8200, announced on the network (SSDP), named "VPlayer test".
mkdir -p /var/cache/minidlna
cat > /etc/minidlna.conf <<CONF
media_dir=/share
port=8200
friendly_name=VPlayer test
db_dir=/var/cache/minidlna
inotify=no
CONF
exec minidlnad -d -f /etc/minidlna.conf -R
