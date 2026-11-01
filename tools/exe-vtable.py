#!/usr/bin/env python3
"""exe-vtable.py IMAGE CLASS [slot_hex...]: MSVC RTTI lookup in a raw-offset=RVA image (exe-dearxan output).
Prints the vftable VA(s) of CLASS (e.g. PlayerIns) and the target of each requested slot offset."""
import struct, sys
BASE = 0x140000000
img = open(sys.argv[1], 'rb').read()
name = b'.?AV' + sys.argv[2].encode() + b'@'
q = lambda o: struct.unpack_from('<Q', img, o)[0]
d = lambda o: struct.unpack_from('<I', img, o)[0]
i = img.find(name)
while i >= 0:
    td = i - 0x10                              # TypeDescriptor: vfptr, spare, name
    tdr = struct.pack('<I', td)
    j = img.find(tdr)
    while j >= 0:
        col = j - 12                            # COL: sig=1, offset, cdOffset, pTypeDesc, pClassDesc, pSelf
        if d(col) == 1 and d(col + 20) == col:
            colva = struct.pack('<Q', BASE + col)
            k = img.find(colva)
            while k >= 0:
                vt = k + 8
                print(f'{img[i:img.find(b"\0", i)].decode()} offset {d(col+4):#x} vftable {BASE+vt:#x}')
                for s in sys.argv[3:]:
                    print(f'  +{s}: {q(vt + int(s, 16)):#x}')
                k = img.find(colva, k + 1)
        j = img.find(tdr, j + 1)
    i = img.find(name, i + 1)
