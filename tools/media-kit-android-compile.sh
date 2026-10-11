#!/usr/bin/env bash
# Compile the current MediaCodec-enabled source for Android. This limited
# static-library configuration is not a full product mpv/JAR/APK build.
set -euo pipefail
root=$(git rev-parse --show-toplevel)
output=${1:?usage: media-kit-android-compile.sh OUTPUT [ANDROID_API]}
api=${2:-24}
[[ "$api" =~ ^[0-9]+$ ]] || { echo 'Invalid Android API' >&2; exit 2; }
mkdir -p "$output"
output=$(cd "$output" && pwd)
sdk=${ANDROID_SDK_ROOT:-${ANDROID_HOME:-}}
test -n "$sdk" || { echo 'Android SDK root is unavailable' >&2; exit 2; }
ndk=${ANDROID_NDK_HOME:-${ANDROID_NDK_LATEST_HOME:-}}
if [[ -z "$ndk" ]]; then
  ndk=$(find "$sdk/ndk" -mindepth 1 -maxdepth 1 -type d | sort -V | tail -1)
fi
test -n "$ndk"
bin="$ndk/toolchains/llvm/prebuilt/linux-x86_64/bin"
cc="$bin/aarch64-linux-android${api}-clang"
test -x "$cc" || { echo "Missing Android compiler: $cc" >&2; exit 2; }
{
  printf 'scope=android-mediacodec-static-compile\nsource=%s\napi=%s\nndk=%s\n' "$(git -C "$root" rev-parse HEAD)" "$api" "$ndk"
  cat "$ndk/source.properties"
  "$cc" --version
  printf 'This is not full product native or device acceptance.\n'
} > "$output/provenance.txt"
build="$output/build"
mkdir "$build"
cd "$build"
"$root/configure" \
  --target-os=android --arch=aarch64 --enable-cross-compile \
  --cc="$cc" --cxx="$bin/aarch64-linux-android${api}-clang++" \
  --ar="$bin/llvm-ar" --nm="$bin/llvm-nm" --ranlib="$bin/llvm-ranlib" \
  --strip="$bin/llvm-strip" --enable-pic \
  --disable-autodetect --disable-programs --disable-doc --disable-debug \
  --disable-everything --disable-avdevice --disable-avfilter \
  --disable-swscale --disable-swresample --disable-shared --enable-static \
  --enable-jni --enable-mediacodec \
  --enable-decoder=h264_mediacodec,hevc_mediacodec \
  > "$output/configure.log" 2>&1
make -j2 libavcodec/libavcodec.a libavutil/libavutil.a > "$output/compile.log" 2>&1
for object in mediacodec_wrapper mediacodecdec_common mediacodecdec; do
  test -s "libavcodec/$object.o"
  "$bin/llvm-readelf" -h "libavcodec/$object.o" > "$output/$object.elf.txt"
  grep -q 'AArch64' "$output/$object.elf.txt"
done
grep -q '^#define CONFIG_HEVC_MEDIACODEC_DECODER 1' config_components.h
grep -q '^#define CONFIG_H264_MEDIACODEC_DECODER 1' config_components.h
cp config.h config_components.h "$output/"
cp ffbuild/config.mak "$output/config.mak"
sha256sum libavcodec/libavcodec.a libavutil/libavutil.a > "$output/static-libraries.sha256"
git -C "$root" diff --exit-code
printf 'Current source Android MediaCodec static compilation passed.\n'
