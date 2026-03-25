#!/bin/bash
./dump_fb $1 $2
ffmpeg -vcodec rawvideo -f rawvideo -pix_fmt bgra -s 1920x1200 -i /tmp/scanout.raw /tmp/scanout.png
rm /tmp/scanout.raw
mv /tmp/scanout.png .
