# Windows installer

An NSIS setup exe carrying `mcu-studio.exe` and everything it needs at runtime:
the Qt DLLs, the MinGW runtime, and the Qt image-format plugins. CI builds it on
every push (see `.github/workflows/build.yml`); releases attach it
automatically when a `vX.Y.Z` tag is pushed.

## Building locally (MSYS2 MINGW64 shell)

```bash
pacman -S mingw-w64-x86_64-{gcc,cmake,ninja,qt6-base,qt6-tools,libjpeg-turbo,nsis}
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build                    # also populates build/dist via `deploy`
cmake --build build --target installer # -> build/McuStudio-<version>-Setup.exe
```

Outside MSYS2, install NSIS so `makensis` is on `PATH`.

## How it fits together

- `windows.cmake` — included by the top-level CMakeLists on Windows. The
  `deploy` target copies the exe into `build/dist/` and resolves its Qt DLLs
  (windeployqt) and MinGW/third-party DLLs (ldd). The `installer` target runs
  `build_installer.cmake`.
- `build_installer.cmake` — script mode; finds `makensis` and compiles
  `mcu-studio.nsi`, passing the version and every path (`dist/`, the LICENSE,
  the icon, the output exe) as `/D` defines. Writes only into the build dir.
- `mcu-studio.nsi` — the installer script: installs `dist/` into
  `Program Files\MCU Studio`, shows the GPLv3 license, creates the Start Menu
  and desktop shortcuts, adds an "Open with MCU Studio" entry to the JPEG
  context menu, registers an Add/Remove Programs entry, and writes the
  uninstaller.

## Why the ldd sweep covers the plugins too

`windeployqt` stages Qt's own DLLs and plugins, but not the third-party
libraries those plugins link. `imageformats/qjpeg.dll` and its neighbours pull
in libjpeg, libwebp and libtiff, which are reachable only through the plugin and
never through `mcu-studio.exe` itself. If they are missing, the plugin fails to
load silently — `QImage` still decodes PNG (built into Qt6Gui) but returns null
for everything else, which in this app means the reference-image features quietly
stop accepting most formats. The `deploy` target therefore runs `ldd` over every
deployed exe *and* DLL, loops to a fixpoint so freshly copied dependencies get
their own dependencies pulled in, and lands them all in `dist/` (Windows resolves
a plugin's DLLs from the process exe's directory).

## File association

The installer does not make MCU Studio the default JPEG handler. It adds a verb
under `HKCR\SystemFileAssociations\.jpg` (and `.jpeg`) so "Open with MCU Studio"
appears on the context menu, which is the right weight for a tool reached for
when a photo is already broken.
