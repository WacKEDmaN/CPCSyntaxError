# SDL2

SDL2 is not included in this repository. To build:

1. Download the SDL2 MinGW development package, `SDL2-devel-2.x.x-mingw.zip`, from
   <https://github.com/libsdl-org/SDL/releases> (SDL 2.x, not SDL 3).
2. Copy the contents of its `x86_64-w64-mingw32` folder (`bin`, `include`, `lib`, ...)
   into this folder, so that `third_party/SDL2/include/SDL2/SDL.h` exists.

Alternatively leave this folder empty and configure with
`-DSDL2_ROOT=<path to the package's x86_64-w64-mingw32 folder>`.

SDL2 is zlib-licensed; its `SDL2.dll` is shipped beside `cpcse.exe` in binary releases.
