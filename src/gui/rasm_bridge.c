/* CPCSyntaxError GUI — the one C file between the emulator and RASM.
 *
 * RASM is a command-line assembler at heart: on a fatal condition (an unsupported path,
 * a bank out of range, an unreadable file) it calls exit(). Inside the emulator that
 * would close the whole program, so rasm.c is compiled with exit() renamed to
 * rasm_bridge_exit(), which jumps back here and reports the failure instead. What RASM
 * had allocated on the way is left behind; a fatal assembly is rare and one-off.
 *
 * The error limit is switched off (maxerr 0 = none): on reaching it rasm frees its
 * whole state and then reads the error count out of the freed block to pass to exit(),
 * which a command-line process survives and a long-lived one does not. */
#include <setjmp.h>
#include <stdlib.h>
#include <string.h>
#include "rasm.h"
#include "rasm_bridge.h"

static jmp_buf rasm_bridge_jump;
static volatile int rasm_bridge_armed = 0;
static volatile int rasm_bridge_code = 0;

__attribute__((noreturn)) void rasm_bridge_exit(int code) {
    if (rasm_bridge_armed) {
        rasm_bridge_code = code ? code : 1;
        longjmp(rasm_bridge_jump, 1);
    }
    exit(code);
}

int rasm_bridge_assemble(const char* source, int length, int syntax,
                         unsigned char** out, int* outLength, struct s_rasm_info** info, int* fatal) {
    static struct s_parameter param;
    int ret;
    memset(&param, 0, sizeof(param));
    /* rasm's own command-line defaults (main()), less the error limit */
    param.web_host = "127.0.0.1";
    param.web_port = 6128;
    param.maxerr = 0;
    param.rough = 0.5f;
    param.module_separator = '_';
    switch (syntax) {
        case RASM_SYNTAX_MAXAM: param.rough = 0.0f; break;   /* -m: Maxam's left-to-right expressions */
        case RASM_SYNTAX_AS80:  param.as80 = 1; break;       /* -ass */
        case RASM_SYNTAX_UZ80:  param.as80 = 2; break;       /* -uz */
        case RASM_SYNTAX_DAMS:  param.dams = 1; break;       /* -dams */
        case RASM_SYNTAX_PASMO: param.pasmo = 1; break;      /* -pasmo */
        default: break;
    }
    *fatal = 0;
    *out = NULL; *outLength = 0; *info = NULL;
    rasm_bridge_code = 0;
    rasm_bridge_armed = 1;
    if (setjmp(rasm_bridge_jump)) {
        rasm_bridge_armed = 0;
        *fatal = rasm_bridge_code;
        return -1;
    }
    ret = RasmAssembleInfoParam(source, length, out, outLength, info, &param);
    rasm_bridge_armed = 0;
    return ret;
}

void rasm_bridge_free(unsigned char* out, struct s_rasm_info* info) {
    if (info) RasmFreeInfoStruct(info);
    if (out) free(out);
}
