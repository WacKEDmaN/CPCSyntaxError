// CPCSyntaxError — debugger tooling helpers.
//
// Byte-level export helpers for the debugging tools. The interactive debugger is
// the GUI's (src/gui/gui_debugger.*); the disassembler, assembler, symbol table,
// breakpoint conditions, DSK filesystem and SNA codec are their own modules.
//   * zipCrc32 / buildZipArchive — STORE-method .zip packaging of exported files
//   * regionFileBaseName        — multi-region AMSDOS output naming, mirroring
//                                 the assembler's WRITE-splitting convention
#pragma once
#include "common.h"

namespace cpcse {

// A file destined for a ZIP archive.
struct ZipEntry { std::string name; Bytes bytes; };

// CRC32 (IEEE 802.3 / zlib polynomial), as used by ZIP entries.
uint32_t zipCrc32(const Bytes& bytes);

// Builds a ZIP archive with the STORE method (no compression, so the packaged
// bytes are exact). Returns the archive bytes.
Bytes buildZipArchive(const std::vector<ZipEntry>& files);

// AMSDOS file name for a multi-region export. C0 (base) regions use the base
// name with numbered suffixes (PROG.BIN, PROG.BI1, PROG.BI2 …); 128K bank
// regions use a per-bank tag (PROG.BC4, PROG.BC5 …). `bank` is the Gate-Array
// config byte (0xC0 base, 0xC4..0xC7 banks); `count` is the 0-based number of
// earlier regions of the same bank.
std::string regionFileBaseName(const std::string& name, int count, int bank);

} // namespace cpcse
