/* CPCSyntaxError: given to rasm.c with -include on MinGW only (CMakeLists.txt).
   rasm.c declares salvador's two functions under #ifndef _WIN32, and MinGW defines _WIN32,
   so GCC 14 and later stop on the implicit declarations. rasm.c stays as upstream has it. */
#ifndef CPCSE_RASM_MINGW_H
#define CPCSE_RASM_MINGW_H
#include <stddef.h>
size_t salvador_compress(const unsigned char *pInputData, unsigned char *pOutBuffer, const size_t nInputSize, const size_t nMaxOutBufferSize,
   const unsigned int nFlags, const size_t nMaxOffset, const size_t nDictionarySize, void(*progress)(long long nOriginalSize, long long nCompressedSize), void *pStats);
size_t salvador_get_max_compressed_size(const size_t nInputSize);
#endif
