# DLNA, FTP and SFTP servers for testing VPlayer's network support (tools/net-test/run.sh).
FROM --platform=linux/amd64 alpine:3.22
RUN apk add --no-cache minidlna vsftpd openssh-server openssh-sftp-server
COPY entry.sh /entry.sh
ENTRYPOINT ["/bin/sh", "/entry.sh"]
