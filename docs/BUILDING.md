# Building, testing and layout

## Prerequisites

- CMake 3.21+
- A C++17 compiler
- Qt 6.2+ (`Widgets`, `Concurrent`, `Network`, and `Test` for the tests)
- Optional: ONNX Runtime, for AI fill's local model. Without it everything
  else builds and works, and that one model is listed as unavailable.
- libjpeg-turbo with development headers. IJG libjpeg works too, but the
  preview decoder is written to match libjpeg-turbo's output, so with IJG
  libjpeg the preview can differ from the exported file by a level here and there.

<details>
<summary>Installing dependencies</summary>

```sh
# Arch
sudo pacman -S cmake qt6-base libjpeg-turbo

# Debian / Ubuntu
sudo apt install cmake qt6-base-dev libjpeg-turbo8-dev

# Fedora
sudo dnf install cmake qt6-qtbase-devel libjpeg-turbo-devel

# macOS
brew install cmake qt libjpeg-turbo

# Windows (MSYS2 MINGW64)
pacman -S mingw-w64-x86_64-{gcc,cmake,ninja,qt6-base,qt6-tools,libjpeg-turbo}
```

</details>

## Build, test and run

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
./build/bin/mcu-studio [image.jpg]
```

Options: `-DMCU_STUDIO_BUILD_TESTS=OFF`, `-DMCU_STUDIO_BUILD_CLI=OFF`,
`-DMCU_STUDIO_FUZZ=ON` (with clang; see [`fuzz/`](../fuzz/CMakeLists.txt)), and
`-DMCU_STUDIO_ONNX=AUTO|ON|OFF`. ONNX Runtime is found as a system package
(`onnxruntime-cpu` on Arch); to use a release unpacked from the ONNX Runtime
project instead, pass `-DONNXRUNTIME_ROOT=/path/to/it`.

## Testing

`ctest` runs the Qt Test suite in `tests/`. It covers the decoder's agreement with
libjpeg, the operations' semantics and their round trips through export,
trailers and MPF rewriting, STOP/Djvu footers, bitstream maps and edits, the
resync search on synthetic damage, donor splicing, ranking and width detection,
alignment and DC estimation, the document model and project files, Exif
editing and carving, and AI fill's windowing, fitting and confinement to the
selection (with a stand-in model). Set `MCU_STUDIO_LONG_TESTS=1` for a 40-case randomized
resync sweep.

Everything that parses untrusted bytes has a libFuzzer harness in `fuzz/`, and
CI runs each one under AddressSanitizer and UBSan on every push.

## Layout

```
src/                        the Qt 6 application and the repair core (mcu_core)
src/cli/                    mcu-studio-cli
third_party/jpegrepair/     vendored jpegrepair (modified) and the preview decoder
tests/                      the test suite
fuzz/                       libFuzzer harnesses and their seed corpus
packaging/                  desktop entry, icons, AUR PKGBUILD
installer/                  Windows NSIS installer
docs/                       features, how it works, building, screenshots
.github/workflows/          CI and releases
```
