import struct, zlib, sys

KEY = (191, 0, 255)          # Photoroom removal purple -> becomes transparent
THRESH = 110                 # RGB distance: pixels near KEY map to index 0
NCOL = 15                    # subject colors (16 total incl. KEY)

def load_png(path):
    d = open(path,'rb').read()
    pos=8; w=h=ct=None; idat=b''
    while pos < len(d):
        ln=struct.unpack_from('>I',d,pos)[0]; typ=d[pos+4:pos+8]; data=d[pos+8:pos+8+ln]
        if typ==b'IHDR': w,h,bd,ct,cm,f,a=struct.unpack_from('>IIBBBBB',data,0)
        elif typ==b'IDAT': idat+=data
        elif typ==b'IEND': break
        pos+=12+ln
    raw=zlib.decompress(idat)
    ch={2:3,6:4}[ct]; stride=w*ch
    prev=bytearray(stride); out=bytearray(); off=0
    for y in range(h):
        f=raw[off]; off+=1
        line=bytearray(raw[off:off+stride]); off+=stride
        for i in range(stride):
            a=line[i-ch] if i>=ch else 0
            b=prev[i]; c=prev[i-ch] if i>=ch else 0
            if f==0: v=line[i]
            elif f==1: v=(line[i]+a)&0xff
            elif f==2: v=(line[i]+b)&0xff
            elif f==3: v=(line[i]+((a+b)//2))&0xff
            elif f==4:
                pa=a+b-c; paa=abs(pa-a); pab=abs(pa-b); pac=abs(pa-c)
                pr=a if (paa<=pab and paa<=pac) else (b if pab<=pac else c)
                v=(line[i]+pr)&0xff
            else: raise ValueError(f)
            line[i]=v
        out+=line; prev=line
    return w,h,ch,bytes(out)

def dist(p,q): return (p[0]-q[0])**2+(p[1]-q[1])**2+(p[2]-q[2])**2

def median_cut(points, nleaves):
    pts=[list(p) for p in points]
    if len(pts)<=1: return pts
    boxes=[pts]
    while len(boxes)<nleaves and len(boxes)<len(set(map(tuple,pts))):
        # pick box with largest channel span
        bi=None; best=-1; bc=None
        for i,bx in enumerate(boxes):
            if len(bx)<2: continue
            spans=[max(p[c] for p in bx)-min(p[c] for p in bx) for c in range(3)]
            m=max(range(3), key=lambda c: spans[c])
            if spans[m]>best: best=spans[m]; bi=i; bc=m
        if bi is None: break
        bx=boxes.pop(bi)
        bx.sort(key=lambda p:p[bc])
        mid=len(bx)//2
        boxes.append(bx[:mid]); boxes.append(bx[mid:])
    # representative = mean
    reps=[]
    for bx in boxes:
        n=len(bx)
        if n==0: continue
        reps.append((sum(p[0] for p in bx)//n, sum(p[1] for p in bx)//n, sum(p[2] for p in bx)//n))
    return reps

def write_bmp4(path, idx, pal, w=32, h=32):
    # idx: 32x32 list of palette indexes; pal: 16 (r,g,b)
    rowbytes=((w*4+31)//32)*4   # bytes per row (padded to 4)
    data=bytearray()
    for y in range(h-1,-1,-1):          # bottom-up
        row=bytearray(rowbytes)
        for x in range(w):
            v=idx[y][x]
            if x&1: row[x//2]|= (v&0xF)           # right pixel = low nibble
            else:   row[x//2]|= (v&0xF)<<4        # left pixel = high nibble
        data+=row
    # palette entry: B,G,R,0
    pmap=b''.join(struct.pack('<BBBB', c[2],c[1],c[0],0) for c in pal)
    filesz=14+40+16*4+len(data)
    header=b'BM'+struct.pack('<IHHI', filesz,0,0,14+40+16*4)
    dib=struct.pack('<IiiHHIIiiII', 40,w,h,1,4,0,len(data),2835,2835,16,0)
    open(path,'wb').write(header+dib+pmap+bytes(data))

def main():
    w,h,ch,pix=load_png(sys.argv[1])
    size=32
    idx=[[-1]*size for _ in range(size)]
    # downscale area-average then pick nearest pre-color later; first gather non-key averages
    subjs=[]
    coarse=[[(0,0,0) for _ in range(size)] for _ in range(size)]
    for j in range(size):
        for i in range(size):
            x0=i*w//size; x1=((i+1)*w+size-1)//size
            y0=j*h//size; y1=((j+1)*h+size-1)//size
            rs=gs=bs=0; n=0
            for yy in range(y0,y1):
                for xx in range(x0,x1):
                    k=(yy*w+xx)*ch; r,g,b=pix[k],pix[k+1],pix[k+2]
                    rs+=r; gs+=g; bs+=b; n+=1
            coarse[j][i]=(rs//n,gs//n,bs//n)
    for j in range(size):
        for i in range(size):
            c=coarse[j][i]
            if dist(c,KEY)<=THRESH:
                idx[j][i]=0
                continue
            idx[j][i]=-2
            subjs.append(c)
    pal=[KEY]
    if subjs:
        reps=median_cut(subjs, NCOL)
        for c in reps:
            if dist(c,KEY)<=THRESH:   # rarely
                continue
            if any(dist(c,p)<=36 for p in pal): continue
            pal.append(c)
        # nearest remap
        for j in range(size):
            for i in range(size):
                if idx[j][i]==-2:
                    c=coarse[j][i]
                    best=min(range(1,len(pal)), key=lambda k: dist(c,pal[k]))
                    idx[j][i]=best
    while len(pal)<16: pal.append((0,0,0))
    write_bmp4(sys.argv[2], idx, pal)
    used=set(v for row in idx for v in row)
    print("palette:")
    for n,c in enumerate(pal): print(n, c)
    print("used indexes:", sorted(used), "max index", max(used))

main()
