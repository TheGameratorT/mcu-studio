# AppImage

One file for the Linux distributions that have no package:
`McuStudio-<version>-x86_64.AppImage`, carrying the application, the
command-line tool, Qt, libjpeg-turbo and ONNX Runtime. CI builds it on every
push (see `.github/workflows/build.yml`); releases attach it automatically
when a `vX.Y.Z` tag is pushed.

## Building locally

```sh
packaging/appimage/build.sh            # -> build-appimage/McuStudio-<version>-x86_64.AppImage
```

Needs cmake, ninja, curl, Qt 6 with `qmake` on `PATH` and the libjpeg-turbo
headers. The script downloads ONNX Runtime, linuxdeploy and its Qt plugin
into the work directory.

An AppImage built this way is for testing the recipe, not for handing out:
it runs only where glibc is at least as new as the build machine's, which is
why CI builds the released one on Ubuntu 22.04 (glibc 2.35). It also takes
whatever Qt plugins the build machine has. On a KDE desktop that includes the
KDE image-format plugins, and linuxdeploy stops if one of them needs a library
that is not installed (`Could not find dependency: libjxrglue.so.0`); install
the library or build in a container.

## How it fits together

- The program is built and installed into `AppDir/` with the normal install
  rules, which also place the desktop entry and the icon.
- ONNX Runtime is opened at run time and never linked, so linuxdeploy cannot
  find it: the script copies the ONNX Runtime project's own release into
  `AppDir/usr/lib`, where the program looks first (`lib/` beside `bin/`). The
  archive is checked against a SHA-256 kept in the script; update both
  together.
- linuxdeploy and its Qt plugin copy Qt, the Qt plugins and every other
  library the program links, then pack the AppImage. The Wayland and
  offscreen platform plugins are named explicitly because the Qt plugin only
  brings the X11 one.

## The command-line tool

`mcu-studio-cli` is inside the AppImage but the file starts the application.
To use it, unpack the AppImage once and run it from there:

```sh
./McuStudio-*-x86_64.AppImage --appimage-extract
squashfs-root/usr/bin/mcu-studio-cli info photo.jpg
```
