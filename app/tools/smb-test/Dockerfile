# A Samba server for testing VPlayer's SMB support (tools/smb-test/run.sh).
FROM alpine:3.22
RUN apk add --no-cache samba
COPY smb.conf /etc/samba/smb.conf
COPY entry.sh /entry.sh
EXPOSE 445
ENTRYPOINT ["/bin/sh", "/entry.sh"]
