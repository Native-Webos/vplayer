# SPDX-License-Identifier: GPL-3.0-or-later
# inject_cc.py IN.h264 FPS OUT.h264 : adds CEA-608 closed captions (CC1) to an H.264 Annex-B stream
# made with AUDs and no B-frames (decode order = presentation order): an ATSC A/53 "GA94" SEI
# before the first slice of each access unit, one byte pair per frame.
#
# The captions: three pop-on captions (one with accents: the special and extended characters), then
# roll-up lines. The times are on screen as subtitles from the decoder.
import sys


def nals(b):
    out = []
    i = 0
    starts = []
    while True:
        j = b.find(b'\x00\x00\x01', i)
        if j < 0:
            break
        s = j - 1 if j > 0 and b[j - 1] == 0 else j
        starts.append((s, j + 3))
        i = j + 3
    for k, (s, p) in enumerate(starts):
        e = starts[k + 1][0] if k + 1 < len(starts) else len(b)
        out.append(b[p:e])
    return out


def odd(c):
    return c if bin(c).count('1') % 2 else c | 0x80


def ctrl(a, b):  # control codes are sent twice
    return [(odd(a), odd(b)), (odd(a), odd(b))]


def text(s):
    pairs = []
    b = s.encode('latin-1')
    for i in range(0, len(b), 2):
        pairs.append((odd(b[i]), odd(b[i + 1]) if i + 1 < len(b) else 0x80))
    return pairs


def pac(row):  # preamble address code: rows 1-15, indent 0, white (CC1)
    table = {1: (0x11, 0x40), 2: (0x11, 0x60), 3: (0x12, 0x40), 4: (0x12, 0x60), 5: (0x15, 0x40),
             6: (0x15, 0x60), 7: (0x16, 0x40), 8: (0x16, 0x60), 9: (0x17, 0x40), 10: (0x17, 0x60),
             11: (0x10, 0x40), 12: (0x13, 0x40), 13: (0x13, 0x60), 14: (0x14, 0x40), 15: (0x14, 0x60)}
    return ctrl(*table[row])


def pop_on(lines):
    p = ctrl(0x14, 0x20)  # resume caption loading
    p += ctrl(0x14, 0x2E)  # erase non-displayed memory
    for k, line in enumerate(lines):
        p += pac(15 - len(lines) + 1 + k)
        for part in line:
            p += part if isinstance(part, list) else text(part)
    p += ctrl(0x14, 0x2F)  # end of caption: shows now
    return p


def sei(pairs):
    cc = bytearray([0xB5, 0x00, 0x31]) + b'GA94' + bytes([0x03, 0x40 | len(pairs), 0xFF])
    for a, b in pairs:
        cc += bytes([0xFC, a, b])
    cc.append(0xFF)  # marker bits
    payload = bytearray([0x04, len(cc)]) + cc
    payload.append(0x80)  # rbsp trailing bits
    # Emulation prevention.
    out = bytearray([0x06])
    zeros = 0
    for x in payload:
        if zeros >= 2 and x <= 3:
            out.append(3)
            zeros = 0
        out.append(x)
        zeros = zeros + 1 if x == 0 else 0
    return bytes(out)


def main():
    src, fps, dst = sys.argv[1], float(sys.argv[2]), sys.argv[3]
    v = nals(open(src, 'rb').read())
    aus = []
    for n in v:
        if (n[0] & 0x1F) == 9 or not aus:  # access unit delimiter
            aus.append([])
        aus[-1].append(n)
    # What to send at which second.
    e_grave = ctrl(0x11, 0x3A)                     # special: è
    a_acute = text('*')                            # basic set: á
    script = {
        1.0: pop_on([['CLOSED CAPTIONS TEST'], ['pop-on, from 1 s']]),
        5.0: ctrl(0x14, 0x2C),                     # erase displayed memory: clears
        # "café très, España, más": é ñ á from the basic set, è a special character.
        6.0: pop_on([['caf', text('\\'), ' tr', e_grave, 's, Espa', text('~'), 'a, m', a_acute, 's']]),
        10.0: pop_on([['THIRD CAPTION, 10 s TO 14 s']]),
        14.0: ctrl(0x14, 0x2C),
        16.0: ctrl(0x14, 0x25) + pac(15) + text('ROLL-UP LINE ONE (16 s)'),
        19.0: ctrl(0x14, 0x2D) + text('ROLL-UP LINE TWO (19 s)'),
        22.0: ctrl(0x14, 0x2D) + text('ROLL-UP LINE THREE (22 s)'),
        26.0: ctrl(0x14, 0x2C),
    }
    queue = []
    for t in sorted(script):
        queue.append((int(round(t * fps)), script[t]))
    pending = []
    out = bytearray()
    for i, au in enumerate(aus):
        while queue and queue[0][0] <= i:
            pending += queue.pop(0)[1]
        # Two pairs per frame at most (25 fps: 50 pairs a second, beyond the 30 of broadcast; fine).
        send = pending[:2] or [(0x80, 0x80)]
        pending = pending[2:]
        first_slice = next((k for k, n in enumerate(au) if (n[0] & 0x1F) in (1, 5)), len(au))
        for k, n in enumerate(au):
            if k == first_slice:
                out += b'\x00\x00\x00\x01' + sei(send)
            out += b'\x00\x00\x00\x01' + n
    open(dst, 'wb').write(out)
    print(len(aus), 'access units with captions')


main()
