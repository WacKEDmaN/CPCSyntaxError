// CPCSyntaxError GUI — CPCtelera's asset steps, read from a project's cfg/*.mk files and run
// without make or bash.
//
// A CPCtelera project converts its images, maps and music while it builds: lines such as
//   $(eval $(call IMG2SPRITES,img/ctlogo.png,1,G,48,62,$(PALETTE),,src/,hwpalette))
// in the cfg/*.mk files that cfg/build_config.mk includes, made into commands by CPCtelera's
// cfg/modules/*.mk and scripts. This reads those files in the order build_config.mk
// includes them (variables, line joins, comments, $(eval $(call ...)) lines) and does each
// step as the modules do:
//   IMG2SPRITES, IMG2SP (SET_PALETTE_FW, SET_MODE, SET_MASK, SET_OUTPUT, SET_FOLDER,
//                SET_IMG_FORMAT, SET_EXTRAPAR, CONVERT, CONVERT_PALETTE)
//                  img2cpc's conversion (cpct_img.h), as cpct_img2tileset calls it
//   TMX2C        cpct_tmx2csv: the first layer's CSV, tile numbers minus 1, 8 bits or packed
//   TMX2DATA     tmx2data (SET_FOLDER, SET_OUTPUTS c / h, CONVERT): every visible layer, 8 bits
//   AKS2DATA     cpct_aks2c: Arkos Tracker 3's own SongToAkg / SongToAkm / SongToSoundEffects
//                (CPCtelera's tools/ArkosTracker-3), then the script's text edits and BIN2C
//   ADD2PACK, PACKZX0 / PACKZX0B / PACKZX1 / PACKZX1B / PACKZX7B
//                cpct_pack -ghs -gh -gs: the files joined, compressed by CPCtelera's
//                tools/<zx>/bin program when it is built, else by zx_compress.h (same bytes)
//   BIN2C        every src/**/*.bin into a .c beside it (cfg/global_functions.mk)
// A step runs when one of its outputs is missing or older than its inputs (the source
// file and the .mk that names it), or always when forced ("Convert assets").
#pragma once
#include <string>
#include <vector>

#include "c_build.h"

namespace cpcse {

struct AssetResult {
    bool ok = true;
    std::vector<CBuildMessage> messages;
    std::string log;
    int converted = 0, upToDate = 0;
};

AssetResult convertProjectAssets(const std::string& projectRoot, const std::string& srcDir, const std::string& cpctFolder, bool force);

// After the link: the images the project's TARGET (cfg/build_config.mk) asks for, beside
// the project, as CPCtelera names them ($(PROJNAME).dsk / .cdt / .sna). cpct_media.h makes
// the disc (the program, then $(DSKFILESDIR)'s files for $(DSKINC), then FILE2SECTOR's
// data) and the tape (CDTMAN's files, else the program as "Game"). The snapshot needs a
// machine: wantSna and snaPath say that the caller is to make it.
struct MediaResult {
    bool ok = true;
    std::vector<CBuildMessage> messages;
    std::string log;
    std::string dskPath, cdtPath, snaPath;
    bool wantSna = false;
    bool discNeeded = false;      // the disc holds more than the program: put it in drive A to run
};
MediaResult makeProjectMedia(const std::string& projectRoot, const std::string& cpctFolder, const std::string& projName,
                             const std::vector<uint8_t>& bin, int load, int run);

} // namespace cpcse
