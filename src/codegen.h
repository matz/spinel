#ifndef SPINEL_CODEGEN_H
#define SPINEL_CODEGEN_H

#include "node_table.h"

/* Generate the full C translation unit for the program in `nt`.
   Returns a malloc'd NUL-terminated buffer (caller frees). Aborts the
   process with a diagnostic on an unsupported construct. */
char *codegen_program(const NodeTable *nt);

/* --check-traits / --dump-traits (ty_traits_check.c): compare the ty_traits
   table with the functions it summarizes, or print it from them, after the
   program's analysis, then stop */
extern int g_check_traits, g_dump_traits;

/* Write `text` to `path`: 1, or 0 with a warning on stderr */
int write_text_file(const char *path, const char *text);

/* --check-bop-arity: each builtin-op row with a count range of its own
   against the arity table's accepted counts for its class and name; the
   rows outside, reported on stderr */
int builtin_ops_arity_check(void);

#endif
