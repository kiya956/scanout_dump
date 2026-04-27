#!/bin/bash

if [ $# -lt 2 ]; then
    echo "usage: $0 <card_name> <crtc_id>" >&2
    exit 1
fi

FB_INFO=$(./dump_fb "$1" "$2") || exit 1
echo "$FB_INFO"

WIDTH=$(echo "$FB_INFO"  | awk '/^fb:/{split($2,a,"x"); print a[1]}')
HEIGHT=$(echo "$FB_INFO" | awk '/^fb:/{split($2,a,"x"); print a[2]}')
METHOD=$(echo "$FB_INFO" | grep -oP '(?<=method=)\S+')
RAW_FMT=$(echo "$FB_INFO" | grep -oP '(?<=raw_fmt=)\S+')
RAW_PITCH=$(echo "$FB_INFO" | grep -oP '(?<=raw_pitch=)\d+')

if [ "$METHOD" = "egl" ]; then
    # EGL path: output is always linear RGBA8, no tiling/crop needed
    ffmpeg -vcodec rawvideo -f rawvideo -pix_fmt rgba \
        -s "${WIDTH}x${HEIGHT}" -i /tmp/scanout.raw \
        -update 1 /tmp/scanout.png
else
    # Linear path: use DRM fourcc to determine ffmpeg pixel format
    FMT=$(echo "$FB_INFO" | grep -oP '(?<=\bfmt=)\S+' | head -1)
    PITCH=$(echo "$FB_INFO" | grep -oP '(?<!\w)pitch=\K\d+' | head -1)

    case "$FMT" in
        XR24|AR24) FFMT="bgra";      BPP=4 ;;
        XB24|AB24) FFMT="rgba";      BPP=4 ;;
        XR30|AR30) FFMT="x2rgb10le"; BPP=4 ;;
        XB30|AB30) FFMT="x2bgr10le"; BPP=4 ;;
        RG16)      FFMT="rgb565le";  BPP=2 ;;
        BG16)      FFMT="bgr565le";  BPP=2 ;;
        RG24)      FFMT="rgb24";     BPP=3 ;;
        BG24)      FFMT="bgr24";     BPP=3 ;;
        YUYV|YUY2) FFMT="yuyv422";  BPP=2 ;;
        UYVY)      FFMT="uyvy422";   BPP=2 ;;
        NV12)      FFMT="nv12";      BPP=0 ;;
        NV21)      FFMT="nv21";      BPP=0 ;;
        YU12)      FFMT="yuv420p";   BPP=0 ;;
        *)
            echo "Unsupported DRM format: $FMT" >&2
            exit 1
            ;;
    esac

    if [ "$BPP" -gt 0 ] && [ $((PITCH / BPP)) -ne "$WIDTH" ]; then
        STRIDE_PX=$((PITCH / BPP))
        CROP="-vf crop=${WIDTH}:${HEIGHT}:0:0"
        SIZE="${STRIDE_PX}x${HEIGHT}"
    else
        CROP=""
        SIZE="${WIDTH}x${HEIGHT}"
    fi

    ffmpeg -vcodec rawvideo -f rawvideo -pix_fmt "$FFMT" \
        -s "$SIZE" -i /tmp/scanout.raw \
        $CROP -update 1 /tmp/scanout.png
fi

rm /tmp/scanout.raw
mv /tmp/scanout.png .
