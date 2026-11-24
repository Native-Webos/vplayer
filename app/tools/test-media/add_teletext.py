# SPDX-License-Identifier: GPL-3.0-or-later
# add_teletext.py IN.ts OUT.ts : adds a DVB teletext stream (EN 300 472) with subtitle page 888 to an
# MPEG-TS made by FFmpeg, which cannot encode teletext. The PMT gains the stream (type 0x06, a
# teletext descriptor: German, subtitle page 888); its PES packets go in at their time.
#
# The page uses the German national option, so "}" "|" "~" are shown as ü ö ß: "Grüße aus Köln".
# The times are in the text.
import struct
import sys

TT_PID = 0x0105
HAM = [0x15, 0x02, 0x49, 0x5E, 0x64, 0x73, 0x38, 0x2F, 0xD0, 0xC7, 0x8C, 0x9B, 0xA1, 0xB6, 0xFD, 0xEA]


def rev(b):
    return int('{:08b}'.format(b)[::-1], 2)


def odd(c):
    return c if bin(c).count('1') % 2 else c | 0x80


def unit(mag, row, payload40, kind=0x03):
    b = [0xC0 | 7, 0xE4]  # field/line (first field, line 7); framing code
    addr = (mag & 7) | row << 3
    b += [HAM[addr & 15], HAM[addr >> 4]] + payload40
    return bytes([kind, 0x2C, b[0]] + [rev(x) for x in b[1:]])


def header(mag, page, erase, national):
    p = [odd(0x20)] * 40
    p[0] = HAM[page & 15]
    p[1] = HAM[page >> 4]
    p[2] = HAM[0]
    p[3] = HAM[8 if erase else 0]  # C4: erase page
    p[4] = HAM[0]
    p[5] = HAM[8]                  # C6: subtitle
    p[6] = HAM[0]
    p[7] = HAM[(national & 7) << 1]  # C12-C14: national option
    for i, ch in enumerate(b'VPLAYER TT 888'):
        p[8 + i] = odd(ch)
    return unit(mag, 0, p)


def row(mag, n, text):
    p = [odd(0x20)] * 40
    data = [0x0B, 0x0B] + list(text.encode('ascii')) + [0x0A, 0x0A]  # start box twice, end box twice
    for i, ch in enumerate(data):
        p[2 + i] = odd(ch)
    return unit(mag, n, p)


def pes(pts90k, units):
    # EN 300 472: header data length 0x24, PES length so the packet fills whole TS payloads.
    # Units are 46 bytes, as are the header and data_identifier together: whole stuffing units
    # (id 0xFF) fill the last TS payload exactly.
    stuffing = b'\xff\x2c' + b'\xff' * 44
    data = b'\x10' + b''.join(units)
    while (45 + len(data)) % 184:
        data += stuffing
    p = pts90k & ((1 << 33) - 1)
    pts = bytes([0x21 | (p >> 29) & 0x0E, (p >> 22) & 0xFF, 0x01 | (p >> 14) & 0xFE, (p >> 7) & 0xFF, 0x01 | (p << 1) & 0xFE])
    hdr = bytes([0x81, 0x80, 0x24]) + pts + b'\xff' * (0x24 - 5)
    body = hdr + data
    return b'\x00\x00\x01\xbd' + struct.pack('>H', len(body)) + body


def crc32_mpeg(data):
    crc = 0xFFFFFFFF
    for byte in data:
        crc ^= byte << 24
        for _ in range(8):
            crc = (crc << 1) ^ 0x04C11DB7 if crc & 0x80000000 else crc << 1
            crc &= 0xFFFFFFFF
    return crc


def payload_of(pkt):
    afc = (pkt[3] >> 4) & 3
    start = 4 + (1 + pkt[4] if afc & 2 else 0)
    return start if afc & 1 else None


def pts_of(pes_start):
    b = pes_start
    if b[:3] != b'\x00\x00\x01' or not (b[7] & 0x80):
        return None
    t = b[9:14]
    return ((t[0] >> 1) & 7) << 30 | t[1] << 22 | (t[2] >> 1) << 15 | t[3] << 7 | t[4] >> 1


def main():
    src, dst = sys.argv[1], sys.argv[2]
    data = open(src, 'rb').read()
    pkts = [bytearray(data[i:i + 188]) for i in range(0, len(data) - 187, 188)]
    pid = lambda p: ((p[1] & 0x1F) << 8) | p[2]
    # The PMT's PID from the PAT, and the video PID (the first stream in the PMT).
    pat = next(p for p in pkts if pid(p) == 0)
    s = payload_of(pat) + 1 + pat[payload_of(pat)]
    pmt_pid = ((pat[s + 10] & 0x1F) << 8) | pat[s + 11]
    first_pts = None
    video_pid = None
    out = []
    script = [
        (2.0, True, ['Teletext page 888, 2 s to 6 s', 'Gr}~e aus K|ln']),
        (6.0, True, []),
        (8.0, True, ['Second subtitle, 8 s to 12 s']),
        (12.0, True, []),
        (14.0, True, ['Third, 14 s to 20 s: [ | ] ~ @']),
        (20.0, True, []),
    ]
    queue = []
    cc = 0
    for p in pkts:
        if pid(p) == pmt_pid and p[1] & 0x40:
            s = payload_of(p) + 1 + p[payload_of(p)]
            sec_len = ((p[s + 1] & 0x0F) << 8) | p[s + 2]
            section = bytes(p[s:s + 3 + sec_len - 4])
            info_len = ((section[10] & 0x0F) << 8) | section[11]
            if video_pid is None:
                e = 12 + info_len
                video_pid = ((section[e + 1] & 0x1F) << 8) | section[e + 2]
            desc = bytes([0x56, 5]) + b'deu' + bytes([(0x02 << 3) | 0, 0x88])  # subtitle page, magazine 8 (0), page 88
            es = bytes([0x06, 0xE0 | TT_PID >> 8, TT_PID & 0xFF, 0xF0, len(desc)]) + desc
            body = section[3:] + es
            new_len = len(body) + 4
            section = bytes([section[0], 0xB0 | (new_len >> 8), new_len & 0xFF]) + body
            section += struct.pack('>I', crc32_mpeg(section))
            q = bytearray(p[:4]) + bytes([0]) + section
            q += b'\xff' * (188 - len(q))
            q[3] = (q[3] & 0xCF) | 0x10  # payload only
            out.append(bytes(q))
            continue
        out.append(bytes(p))
        if video_pid is not None and pid(p) == video_pid and p[1] & 0x40 and payload_of(p) is not None:
            t = pts_of(bytes(p[payload_of(p):]))
            if t is None:
                continue
            if first_pts is None:
                first_pts = t
                queue = [(first_pts + int(sec * 90000), erase, rows) for sec, erase, rows in script]
            # A subtitle goes in about half a second before its time.
            while queue and queue[0][0] <= t + 45000:
                when, erase, rows = queue.pop(0)
                units = [header(8, 0x88, erase, 4)] + [row(8, 20 + 2 * i, r) for i, r in enumerate(rows)]
                pkt_data = pes(when, units)
                for k in range(0, len(pkt_data), 184):
                    chunk = pkt_data[k:k + 184]
                    ts = bytes([0x47, (0x40 if k == 0 else 0) | TT_PID >> 8, TT_PID & 0xFF, 0x10 | cc]) + chunk
                    cc = (cc + 1) & 15
                    out.append(ts + b'\xff' * (188 - len(ts)))
    open(dst, 'wb').write(b''.join(out))
    print('teletext PID 0x%X added, %d subtitles left unplaced' % (TT_PID, len(queue)))


main()
