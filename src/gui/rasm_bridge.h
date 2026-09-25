/* CPCSyntaxError GUI — RASM, called from C++. See rasm_bridge.c. */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

struct s_rasm_info;

enum {
    RASM_SYNTAX_RASM = 0,    /* rasm's defaults */
    RASM_SYNTAX_MAXAM = 1,   /* -m */
    RASM_SYNTAX_AS80 = 2,    /* -ass */
    RASM_SYNTAX_UZ80 = 3,    /* -uz */
    RASM_SYNTAX_DAMS = 4,    /* -dams */
    RASM_SYNTAX_PASMO = 5    /* -pasmo */
};

/* Returns rasm's result (0 = assembled). *fatal is non-zero when rasm gave up and
 * called exit(); the result is then -1 and *info may be NULL. */
int rasm_bridge_assemble(const char* source, int length, int syntax,
                         unsigned char** out, int* outLength, struct s_rasm_info** info, int* fatal);
void rasm_bridge_free(unsigned char* out, struct s_rasm_info* info);

#ifdef __cplusplus
}
#endif
