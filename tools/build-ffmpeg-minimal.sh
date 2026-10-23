#!/usr/bin/env bash
# Builds a small static FFmpeg with only what serconv uses: the MOV, MLV, AVI,
# NUT and Matroska demuxers and the ProRes RAW, CineForm, raw video, TIFF and
# MJPEG decoders (LGPL, no external libraries). Linux and macOS.
#
#   tools/build-ffmpeg-minimal.sh [prefix] [tag]
#
# prefix defaults to ./deps/ffmpeg, tag to n9.0.2. Then configure serconv with
#
#   PKG_CONFIG_PATH=<prefix>/lib/pkgconfig cmake -S . -B build -DSERCONV_FFMPEG=ON
set -euo pipefail

prefix=${1:-$PWD/deps/ffmpeg}
tag=${2:-n9.0.2}
case "$prefix" in
  /*) ;;
  *) prefix="$PWD/$prefix" ;;
esac

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

git clone --quiet --depth 1 --branch "$tag" https://github.com/FFmpeg/FFmpeg.git "$work/ffmpeg"
cd "$work/ffmpeg"

asm=""
if ! command -v nasm > /dev/null 2>&1; then
  case "$(uname -m)" in
    x86_64|i?86) asm="--disable-x86asm"; echo "nasm not found: building without x86 assembly (slower)" ;;
  esac
fi

./configure --prefix="$prefix" \
  --disable-everything --disable-programs --disable-doc --disable-network --disable-autodetect \
  --enable-static --disable-shared --enable-pic \
  --disable-avdevice --disable-avfilter --disable-swscale --disable-swresample \
  --enable-protocol=file \
  --enable-demuxer=mov,mlv,avi,nut,matroska \
  --enable-decoder=prores_raw,cfhd,rawvideo,tiff,mjpeg \
  $asm

make -j"$(getconf _NPROCESSORS_ONLN 2> /dev/null || echo 2)"
make install
echo "FFmpeg $tag installed in $prefix"
echo "Configure serconv with: PKG_CONFIG_PATH=$prefix/lib/pkgconfig cmake -S . -B build -DSERCONV_FFMPEG=ON"
