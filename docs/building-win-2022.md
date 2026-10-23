# Building on Windows with Visual Studio 2022

This checkout is built here with **Visual Studio 2022**, not Visual Studio 2026 as
[building-win.md](building-win.md) describes. Everything else in that guide applies.

## Toolchain

- Visual Studio 2022 Community with MSVC toolset **14.44** (v143), so
  `-vcvars_ver=14.44` selects it correctly.
- Windows SDK **10.0.26100.0** is required. SDK 10.0.19041 fails while building
  FFmpeg: its `d3d12va_decode.c` calls the Direct3D 12 resource description
  getter with an output parameter, and that shape only exists in the newer SDK.
- CMake and Ninja ship inside Visual Studio, under
  `Common7\IDE\CommonExtensions\Microsoft\CMake\`. Neither is on PATH by default.
  Add both directories (`CMake\bin` and `Ninja`) to PATH before preparing
  dependencies.

## Dependency preparation

From the folder that contains the checkout, in a 64-bit developer prompt:

    tdesktop\Telegram\build\prepare\win.bat

The preparation script needs `cmake` and `ninja` on PATH as described above.

### JPEG-XL include order fix

`Libraries\win64\libjxl\lib\jxl\base\common.h` includes `<intrin.h>` before
`compiler_specific.h`, but the guard on that include is defined inside
`compiler_specific.h`. With toolset 14.44 the include is skipped and the build
stops on an unknown `_sub_overflow_i32` identifier. Move the
`compiler_specific.h` include above the `<intrin.h>` block, the order that
`bits.h` and `byte_order.h` already use in the same folder.

Preparation deletes and re-clones a stage folder whose cache key is stale, which
discards that fix. To continue without losing it, name the remaining stages
explicitly; preparation then touches only the stages named:

    ...\prepare\win.bat silent ffmpeg libheif openal-soft breakpad qt6_windows7 tg_angle qt_6.11.2 tg_owt ada tde2e tlottie wallet-engine tdesktop_rust

## Configure and build

    cd tdesktop\Telegram
    configure.bat x64 -D TDESKTOP_API_ID=<your id> -D TDESKTOP_API_HASH=<your hash>
    cd ..
    cmake --build out --config Debug --target Telegram

The executable lands in `out\Debug\Telegram.exe`.

API credentials are passed on the configure line only. They end up in the
generated build files under `out` and are never written into tracked sources.
