# compares two float32 mono wavs written by devrev_cli (stdlib only)
import struct, sys, math
def rd(p):
    b=open(p,'rb').read(); i=12
    while i < len(b):
        cid=b[i:i+4]; sz=struct.unpack('<I',b[i+4:i+8])[0]
        if cid==b'data': return struct.unpack('<%df'%(sz//4), b[i+8:i+8+sz])
        i+=8+sz+(sz&1)
a=rd(sys.argv[1]); c=rd(sys.argv[2]); n=min(len(a),len(c))
e=sum((a[i]-c[i])**2 for i in range(n)); s=sum(c[i]**2 for i in range(n))
mx=max(abs(a[i]-c[i]) for i in range(n)); rel=10*math.log10(e/s+1e-30)
print('max diff %.3g, difference energy %.1f dB below signal'%(mx,rel))
sys.exit(0 if rel < -60 else 1)
