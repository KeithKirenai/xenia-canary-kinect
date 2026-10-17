import array, sys

BUF = 0x83D30000
GUEST_BASE = 0x82000000
MASK = 0x5A000000

def load(path):
    a = array.array('I')
    with open(path, 'rb') as f:
        a.frombytes(f.read())
    if sys.byteorder == 'little':
        a.byteswap()
    return a

def find_runs(vals):
    runs = []
    n = len(vals)
    i = 0
    while i < n:
        v = vals[i]
        if (v & 0xFF000000) == MASK and 0 < (v & 0xFFFF) < 0x2000:
            start = i
            i += 1
            while i < n and vals[i] == (MASK | (i - start)):
                i += 1
            runs.append((start, i - start))
        else:
            i += 1
    return runs

def main():
    path = sys.argv[1] if len(sys.argv) > 1 else 'guest_probe_b.bin'
    vals = load(path)
    buf_idx = (BUF - GUEST_BASE) // 4
    runs = find_runs(vals)
    print(f'{path}: {len(runs)} sentinel runs')
    print(f'  our own buffer 0x{BUF:08X} -> index {buf_idx}')
    for s, l in runs:
        addr = GUEST_BASE + s * 4
        tag = 'OUR-BUFFER' if addr == BUF else ''
        print(f'  0x{addr:08X} len={l:5d} dwords ({l*4:6d} B) '
              f'src_off=0x{(s - buf_idx) * 4:<5X} {tag}')

main()