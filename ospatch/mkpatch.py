import struct,sys,hashlib
src,stubf,dst=sys.argv[1:4]
img=bytearray(open(src,'rb').read())
assert hashlib.md5(img).hexdigest()=='be2bca20064a5f6e3e2c474d9ec2819c'
def off(a): return 0x800+a-0x07ff5128
def rd(a): return struct.unpack_from('<I',img,off(a))[0]
def wr(a,v,old):
    assert rd(a)==old,(hex(a),hex(rd(a)),hex(old)); struct.pack_into('<I',img,off(a),v)
def bl(at,to,cond=0xe,link=1): return (cond<<28)|(0x0b000000 if link else 0x0a000000)|(((to-at-8)>>2)&0xffffff)
wr(0x81ff0ac, bl(0x81ff0ac,0x81ff11c,link=0), 0xea000012)     # .pcm reader -> unsupported
wr(0x8087260, bl(0x8087260,0x8087214,cond=0,link=0), 0x0a000005)  # ".flac" match -> WAV type 3
wr(0x8087264, 0xe3a02003, 0xe3a02004)                            # aif/aifc compare: 3 characters
o=off(0x80872f0); assert img[o:o+5]==b'aiff\0'; img[o:o+4]=b'flac'  # 'aiff' type string -> 'flac'
wr(0x828a96c, bl(0x828a96c,0x8277cd4), bl(0x828a96c,0x81ea554))  # WAV open: File constructor, replaced with flac_hook below
o=0x800+0x7fee84; assert img[o:o+13]==b'Shuffle Songs'; img[o:o+13]=b'Shuffle FLACs'   # rename the Shuffle Songs row
stub=open(stubf,'rb').read()
sym={l.split()[2]:int(l.split()[0],16) for l in open(sys.argv[4]) if len(l.split())==3}
BASE=0x8277bb8; END=0x8278144                                             # stub area in the OS image
assert len(stub)<=END-BASE, len(stub)
wr(0x828a96c, bl(0x828a96c,sym['flac_hook']), bl(0x828a96c,0x8277cd4))   # WAV open -> flac_hook
def rb(at,old,new):   # retarget any b/bl at 'at' from old to new, keeping cond and link
    v=rd(at); assert (v&0x0e000000)==0x0a000000 and ((v+0)&0xffffff)==(((old-at-8)>>2)&0xffffff),(hex(at),hex(v))
    wr(at,(v&0xff000000)|(((new-at-8)>>2)&0xffffff),v)
for at in (0x804647c,0x80464b0,0x80464cc,0x8092e40,0x80931c0,0x80c70b8,0x8299628): rb(at,0x80457e4,sym['art_find'])
for at in (0x81aa97c,0x8264ab4,0x8264b78,0x8264d20,0x82bd214): rb(at,0x80f32ec,sym['art_load'])
wr(0x80da098, bl(0x80da098,sym['loc_db']), bl(0x80da098,0x80dc410))                 # db location -> record
for at in (0x805f738,0x80d448c): wr(at, bl(at,sym['loc_set']), bl(at,0x804fde0))     # path -> record
for at in (0x8048288,0x80cc33c,0x80d7fa0,0x80d7fdc,0x80da17c):
    wr(at, bl(at,sym['loc_path']), bl(at,0x80605c8))                                 # record -> path
# no ArtworkDB (fnfErr -43): keep the artwork library empty, not discarded
wr(0x804d434, bl(0x804d434,0x804d43c,cond=0,link=0), bl(0x804d434,0x804d44c,cond=0,link=0))
# no iPod_Control/Artwork folder: still build the empty artwork library
wr(0x804d3d0, 0xe1a00000, 0x1a00001d)                                  # stat Artwork dir failed -> carry on
wr(0x804d410, 0xe1a00000, 0x1a00000d)                                  # stat ArtworkDB failed: still try the load
o=off(BASE); img[o:o+len(stub)]=stub
o=off(END); img[o:o+4]=b'\0\0\0\0'      # zero word at END, just past the stub area
open(dst,'wb').write(img); print('ok',hashlib.md5(img).hexdigest())
