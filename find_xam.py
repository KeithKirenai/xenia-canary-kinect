import struct
BASE = 0x84000000
data = open('guest_xam_hi.bin','rb').read()
target = 0x0002B001
hits = []
for off in range(0, len(data)-8, 4):
    w = struct.unpack('>I', data[off:off+4])[0]
    if (w >> 26) in (15, 25):
        hi = (w & 0xFFFF) << 16
        if hi == (target & 0xFFFF0000):
            for d in (4, 8, 12, 16):
                if off+d+4 > len(data): break
                w2 = struct.unpack('>I', data[off+d:off+d+4])[0]
                if (w2 >> 26) in (14, 24, 25) and ((w2 & 0xFFFF) | hi) == target and ((w2 >> 1) & 0x1F) == ((w >> 21) & 0x1F):
                    hits.append(BASE + off)
                    break
print('refs to 0x2B001 in 0x84000000-0x85FFFFFF:', len(hits))
for h in hits[:10]:
    print('  0x%08X' % h)
