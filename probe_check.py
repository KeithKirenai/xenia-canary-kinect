import array, sys
GUEST_BASE=0x82000000; BUF=0x83D30000; MASK=0x5A000000
buf_idx=(BUF-GUEST_BASE)//4
for path in ('guest_probe_a.bin','guest_probe_b.bin'):
    a=array.array('I')
    with open(path,'rb') as f: a.frombytes(f.read())
    if sys.byteorder=='little': a.byteswap()
    print(path)
    print('  first dwords at buf:', [hex(a[buf_idx+i]) for i in range(8)])
    print('  dwords at +48:', [hex(a[buf_idx+12+i]) for i in range(6)])
    n=len(a)
    runs=[];i=0
    while i<n:
        v=a[i]
        if (v & 0xFF000000)==MASK and 0<(v & 0xFFFF)<0x2000:
            s=i;i+=1
            while i<n and a[i]==(MASK|(i-s)): i+=1
            runs.append((s,i-s))
        else: i+=1
    big=[(s,l) for s,l in runs if l>=4]
    print(f'  runs>=4 dwords: {len(big)}')
    for s,l in big:
        print(f'    0x{GUEST_BASE+s*4:08X} len={l} ({l*4}B) src_off={(s-buf_idx)*4}')
    print()
