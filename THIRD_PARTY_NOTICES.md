# Third-party notices

CPCSyntaxError's own code is MIT-licensed (`LICENSE`). It includes, or links, the
components below, which keep their own licences. Their full licence texts are in the
files named here.

## Included in the source tree

| Component | Version | Author | Licence | Where |
|---|---|---|---|---|
| Dear ImGui (docking branch) | 1.92.9b | Omar Cornut and contributors | MIT | `third_party/imgui/LICENSE.txt` |
| ymfm (OPL family: the YMF278B / OPL4) | upstream 81aec25 | Aaron Giles | BSD 3-Clause | `third_party/ymfm/LICENSE` |
| RASM | 3.3 (upstream 75f4e908) | Edouard BERGE | MIT | `third_party/rasm/LICENSE` |
| apultra (RASM cruncher) | as shipped with RASM | Emmanuel Marty | zlib | header of `third_party/rasm/apultra-master/src/*.h` |
| LZSA (RASM cruncher) | as shipped with RASM | Emmanuel Marty | zlib | header of `third_party/rasm/lzsa-master/src/*.h` |
| salvador (RASM cruncher) | as shipped with RASM | Emmanuel Marty | zlib | header of `third_party/rasm/salvador/src/*.h` |
| libdivsufsort | as shipped with RASM | Yuta Mori | MIT | `third_party/rasm/apultra-master/src/libdivsufsort/LICENSE` |
| LZ4 (in `lz4.h`) | as shipped with RASM | Yann Collet | BSD 2-Clause | header of `third_party/rasm/lz4.h` |
| ZX7 (in `zx7.h`) | as shipped with RASM (modified by the RASM author) | Einar Saukas | BSD 3-Clause | header of `third_party/rasm/zx7.h` |
| **Exomizer** (in `exomizer.h`) | as shipped with RASM | Magnus Lind | **non-commercial, non-profit use only** | header of `third_party/rasm/exomizer.h` |

**Exomizer's licence restricts the whole assembler build to non-commercial,
non-profit use.** Anyone wanting to use CPCSyntaxError commercially must rebuild
RASM without its third-party crunchers (`NO_3RD_PARTIES`), which removes Exomizer
along with the others.

## Linked into the Windows binary

| Component | Licence | Notes |
|---|---|---|
| SDL2 | zlib | shipped as `SDL2.dll`; licence in `LICENSE-SDL2.txt` beside it |
| GCC runtime (libstdc++, libgcc) | GPL v3 with the GCC Runtime Library Exception | linked statically; the exception allows this without further obligations |
| MinGW-w64 runtime and winpthreads | public domain / MIT-style (mingw-w64 licences) | linked statically |

## Documentation and data

- **"Amstrad CPC CRTC Compendium"** (ACCC) by Longshot, CC BY-NC-ND 4.0. The CRTC,
  Gate Array and monitor code quotes short passages of it in comments, with chapter
  and page references, to show where each rule comes from. Those passages remain
  Longshot's and are under his licence. The compendium itself is not included.
- **CP/M 2.2 system image** (`src/core/cpm22.cpp`): the boot sectors and BDOS of an
  Amstrad CP/M 2.2 system disc. CP/M 2.2 is Digital Research's, later made freely
  redistributable. The Amstrad loader is Amstrad's; Amstrad allows its CPC system
  software to be distributed for emulation and keeps the copyright.
- **Amstrad CPC firmware ROMs and the CPC Plus / GX4000 system cartridge**
  (`roms/`: CPC 464 and 6128 OS and BASIC, AMSDOS, and the "Basic - Burning Rubber"
  system cartridge). Amstrad have kindly given their permission for the redistribution
  of their copyrighted material but retain that copyright; the game Burning Rubber on
  the cartridge remains the copyright of its owners. None of these are covered by
  CPCSyntaxError's MIT licence.
- No other software is included.
