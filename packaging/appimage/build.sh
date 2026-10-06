#!/bin/sh
# Builds MCU Studio and packs it as an AppImage: one file that carries the
# program, the command-line tool, Qt, libjpeg-turbo and ONNX Runtime, and runs
# on any x86_64 distribution whose glibc is at least as new as this machine's.
# CI runs it on the oldest Ubuntu GitHub offers for that reason (see
# .github/workflows/build.yml).
#
#   packaging/appimage/build.sh [work-dir]
#
# Needs cmake, ninja, curl, Qt 6 with qmake on PATH (or QMAKE set) and the
# libjpeg-turbo headers. Everything is written under work-dir, by default
# build-appimage/ in the source tree; the AppImage lands there.
set -eu

root=$(cd "$(dirname "$0")/../.." && pwd)
work=${1:-$root/build-appimage}
mkdir -p "$work"
work=$(cd "$work" && pwd)
cd "$work"

# ONNX Runtime as its own project releases it, for the processor only: a
# distribution's package cannot be relied on to exist, and this one needs
# nothing but glibc. The program opens it at run time from lib/ beside bin/.
ort_version=1.30.0
ort_sha256=a5ed5a3cac51fbb2e90da632ae43d19212faaa20e76484e62bcb7c23ddb3b3fd
ort=onnxruntime-linux-x64-$ort_version
tools=https://github.com/linuxdeploy

fetch() {
    [ -f "$2" ] || curl -fsSL --retry 3 -o "$2" "$1"
}
fetch "https://github.com/microsoft/onnxruntime/releases/download/v$ort_version/$ort.tgz" "$ort.tgz"
echo "$ort_sha256  $ort.tgz" | sha256sum -c -
fetch "$tools/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage" linuxdeploy
fetch "$tools/linuxdeploy-plugin-qt/releases/download/continuous/linuxdeploy-plugin-qt-x86_64.AppImage" \
    linuxdeploy-plugin-qt
chmod +x linuxdeploy linuxdeploy-plugin-qt
rm -rf "$ort" AppDir
tar -xzf "$ort.tgz"

cmake -S "$root" -B build -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=/usr \
    -DMCU_STUDIO_ONNX=ON \
    -DONNXRUNTIME_ROOT="$work/$ort"
cmake --build build
DESTDIR="$work/AppDir" cmake --install build --strip

# Nothing links ONNX Runtime, so linuxdeploy would not find it by itself.
mkdir -p AppDir/usr/lib
cp -P "$ort"/lib/libonnxruntime.so.1* AppDir/usr/lib/
mkdir -p AppDir/usr/share/doc/mcu-studio/onnxruntime
cp "$ort"/LICENSE "$ort"/ThirdPartyNotices.txt AppDir/usr/share/doc/mcu-studio/onnxruntime/

version=$(sed -n 's/^CMAKE_PROJECT_VERSION:STATIC=//p' build/CMakeCache.txt)
out="McuStudio-$version-x86_64.AppImage"
rm -f "$out"

# The tools are AppImages themselves; unpacking instead of mounting lets them
# run where FUSE is missing, which includes CI and containers.
export APPIMAGE_EXTRACT_AND_RUN=1
export QMAKE="${QMAKE:-$(command -v qmake6 || command -v qmake)}"
# The Qt plugin only brings the X11 platform plugin. Wayland's are added,
# looked up because their file names differ between Qt versions, and so is the
# offscreen one, which --screenshot needs on a machine without a display.
export EXTRA_PLATFORM_PLUGINS="$(cd "$("$QMAKE" -query QT_INSTALL_PLUGINS)/platforms" &&
    ls libqwayland*.so libqoffscreen.so | paste -sd ';')"
export EXTRA_QT_MODULES="waylandcompositor"
# linuxdeploy's own strip is too old for libraries from current distributions
# and fails on them; the program was stripped when it was installed above.
export NO_STRIP=1
export LINUXDEPLOY_OUTPUT_VERSION="$version"
export LDAI_OUTPUT="$out"
./linuxdeploy --appdir AppDir --plugin qt --output appimage

echo "Created $work/$out"
