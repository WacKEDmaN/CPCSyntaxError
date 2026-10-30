// CPCSyntaxError GUI — the disc and tape images a CPCtelera project makes of its program
// (its TARGET: $(DSK), $(DSKINC), $(CDT); the snapshot is made by the GUI's machine), as
// CPCtelera's tools make them:
//
//   iDSK -n            a new disc: 42 tracks, one side, 9 sectors &C1-&C9 of 512 bytes,
//                      interleaved C1 C6 C2 C7 C3 C8 C4 C9 C5, GAP#3 &4E, filler &E5
//   iDSK -i f -t 1 ... a file with an AMSDOS header (type 2, binary) unless it has one;
//                      -t 0 (a text file): the bytes alone, no header. The name: the file's
//                      name up to its first '.', 8 characters, and 3 after it, upper case.
//   cpc2cdt            CPC firmware records in TZX turbo blocks (&11): a 2K first block and
//                      one block for the rest (-t), 2000 baud, 4096 pilot pulses, 4 bytes
//                      of trailer, CRC-16 per 256-byte segment; header records paused 16 ms,
//                      data records 2560 ms, the last one -p ms. A new file starts with the
//                      TZX header and a 2560 ms pause block.
//   FILE2SECTOR        RASM's EDSK WRITESECT: a file written into sectors given by location
//                      ("5+:0xC1-0xC9" = track 5 to the last, sectors &C1-&C9 of each).
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace cpcse {

struct Disk;

// A blank disc as iDSK -n makes it.
std::shared_ptr<Disk> cpctNewDisc();
// The image file: the standard format as iDSK writes it, or (after FILE2SECTOR) the
// extended one as RASM writes it back.
std::vector<uint8_t> cpctDiscImage(const Disk& disk);

// iDSK -i: hostName gives the disc name (its file name part). type 1 binary (an AMSDOS header
// is added unless the data has one; load and exec go into it), type 0 text (no header).
// "" or why not.
std::string cpctDiscImport(Disk& disk, const std::string& hostName, const std::vector<uint8_t>& data, int type, int load, int exec);

// RASM's EDSK WRITESECT on side 0: the data from the start of the location list on. "" or
// why not.
std::string cpctDiscWriteSectors(Disk& disk, const std::vector<uint8_t>& data, const std::string& location);

// file's `file | grep ASCII`: the bytes are text (iDSK then adds no header).
bool cpctLooksAscii(const std::vector<uint8_t>& data);

// cpc2cdt into an image (an empty one is started): one CPC file. mode "cpc" or "cpctxt";
// load / run < 0: not given (cpc2cdt's 0x4000, or the AMSDOS header's when the data has
// one); pauseMs = -p (cpc2cdt's default 10240 when < 0). Always -t and -b 2000, as CDTMAN
// calls it.
void cpctTapeAddFile(std::vector<uint8_t>& cdt, const std::vector<uint8_t>& data, const std::string& name, const std::string& mode,
                     int load, int run, int pauseMs);

} // namespace cpcse
