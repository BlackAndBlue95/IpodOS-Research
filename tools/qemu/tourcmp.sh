#!/bin/zsh
# tourcmp.sh BASE_KERNEL VARIANT_KERNEL...  — real-time tours (main menu panes, Music, Songs, Now
# Playing, Settings, Backlight, Extras) right after the first frame; prints differing pixels per screen
cd /Users/harrison/IPOD/build/qemu/run
Q=../src/build/qemu-system-arm
T="shot e wait shot e wait shot e wait shot e wait shot e wait shot e wait shot q q q q q q wait ret wait shot e e e e e e wait ret wait shot ret wait wait wait shot w wait w wait w wait w wait e e e e e wait ret wait shot e e e e e e wait ret wait shot w wait w wait q wait ret wait shot"
i=0
for k in "$@"; do i=$((i+1)); cp EF.img Z-tc$i.img; rm -f montc$i.sock; IPOD_SYSINFO=sysinfo-real.bin $Q -M iPod-Classic -kernel $k -serial null -drive if=ide,format=raw,file=Z-tc$i.img -display none -monitor unix:montc$i.sock,server,nowait >/dev/null 2>&1 & done
for j in $(seq 1 $i); do for w in $(seq 1 240); do python3 /Users/harrison/IPOD/tools/qemu/fatcat.py Z-tc$j.img 'FLAC\rtlog.txt' $((49160*4096)) 2>/dev/null | grep -q "first frame" && break; sleep 0.25; done; done
pids=()
setopt nonomatch; for j in $(seq 1 $i); do rm -f tcs$j-*.ppm; (/Users/harrison/IPOD/tools/qemu/tour.sh montc$j.sock tcs$j ${=T}) & pids+=($!); done
for p in $pids; do while kill -0 $p 2>/dev/null; do sleep 1; done; done
pkill -f "Z-tc[0-9]+\.img"
python3 - $i <<'PY'
import sys
n=int(sys.argv[1]); names=['main1','main2','main3','main4','main5','main6','main7','Music','Songs','NowPlay','Settings','Backlight','Extras']
def load(f):
    d=open(f,'rb').read(); i=d.index(b'255\n')+4; return d[i:]
for v in range(2,n+1):
    out=[]
    for s in range(1,14):
        try:
            a=load('tcs1-%d.ppm'%s); b=load('tcs%d-%d.ppm'%(v,s))
            out.append('%s:%d'%(names[s-1],sum(1 for y in range(20,240) for x in range(320) if a[(y*320+x)*3:(y*320+x)*3+3]!=b[(y*320+x)*3:(y*320+x)*3+3])))
        except Exception as e: out.append('%s:?'%names[s-1])
    print('variant %d: %s'%(v-1,' '.join(out)))
PY
