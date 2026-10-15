#!/bin/sh
# ipodenc.sh IN OUT.m4v [START LENGTH]: Apple's iPod H.264 profile (Baseline 3.0, 640x480, 1.5 Mbit/s,
# AAC 128k 44.1 kHz), constant 29.97 fps, no caption SEI
set -e
FF=$(cd "$(dirname "$0")/../.." && pwd)/build/tools/ffmpeg
IN=${1:?input}; OUT=${2:?output}
TRIM=""; [ -n "$3" ] && TRIM="-ss $3 -t ${4:?length}"
"$FF" -hide_banner -loglevel error -nostdin -y $TRIM -i "$IN" -map 0:v:0 -map 0:a:0 \
    -map_metadata -1 -map_chapters -1 \
    -vf "fps=30000/1001,scale=640:480:force_original_aspect_ratio=decrease,pad=640:480:(ow-iw)/2:(oh-ih)/2,setsar=1,format=yuv420p" \
    -c:v libx264 -preset faster -profile:v baseline -level 3.0 -a53cc 0 \
    -x264-params "ref=1:bframes=0:cabac=0:8x8dct=0:weightp=0:slices=1:keyint=60:min-keyint=30" \
    -b:v 1200k -maxrate 1500k -bufsize 2000k \
    -c:a aac -b:a 128k -ar 44100 -ac 2 \
    -movflags +faststart -f mp4 "$OUT"
