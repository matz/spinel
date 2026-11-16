#ifndef SP_ARGF_H
#define SP_ARGF_H
/* sp_argf.h -- ARGV/ARGF state + cold accessors.
 *
 * sp_argv is populated by the generated main() (codegen emits
 * `sp_argv.len = argc - 1; sp_argv.data = ...` directly), so its storage
 * stays defined in the generated TU -- just non-static now, resolved at
 * the final link the same way lib/sp_core.c reaches sp_sprintf. sp_argf_obj
 * (ARGF's single-instance read cursor) follows the same pattern.
 */
#include "sp_types.h"   /* sp_int */
#include <stdio.h>      /* FILE */

typedef struct{const char**data;sp_int len;}sp_Argv;
extern sp_Argv sp_argv;               /* defined in the generated TU */

typedef struct { FILE *cur; int started; const char *fname; } sp_Argf;
extern sp_Argf sp_argf_obj;            /* defined in the generated TU */

/* ENV: the process environment has no state of its own to hold; the object
   is what the ENV value points at, so that it has an identity, can share a
   slot with nil and boxes under SP_BUILTIN_ENV. */
typedef struct { char unused; } sp_env_object;
extern sp_env_object sp_env_obj;              /* defined in the generated TU */

/* the ARGV-as-poly-array materialization cache: allocated lazily by
   sp_get_ARGV (lib/sp_cold.c), read by sp_re_mark_globals's GC root scan
   (spinel_rt.h) -- extern so both sides see the same object. */
extern sp_StrArray *sp_argv_array_cache;

sp_StrArray *sp_get_ARGV(void);

/* $LOADED_FEATURES (`$"`): an Array of Strings the program reads and pushes
   onto. It starts empty: the files the program requires were compiled into
   it rather than loaded as it runs, so none is listed. Allocated lazily by
   sp_get_loaded_features (lib/sp_cold.c) and marked with the runtime
   globals, as ARGV's array is. */
extern sp_StrArray *sp_loaded_features_cache;
sp_StrArray *sp_get_loaded_features(void);
/* `require path` with a path known only at run time: a feature the program
   was compiled with (the table the compiler emits) is already loaded, and
   anything else raises CRuby's LoadError. A path matches a feature as
   written or as the end of a longer path (`File.join(__dir__, "x")`), with
   or without its ".rb". */
void sp_require_computed(const char *path, const char *const *features, int n);
/* --load-model=graph: `require path` with a path known only at run time
   names a file the program was compiled with, found as CRuby finds one
   (the inline model's sp_require_computed above is deleted with that model).
   A path is
     - absolute: that file;
     - `./` or `../`: relative to the current directory, expanded then;
     - `require_relative`'s (`rel`): relative to `dir`, the requiring file's
       directory;
     - bare: searched along $LOAD_PATH as it stands when the require runs, in
       order. An entry names a file of the program by its absolute path; the
       entry that is the compiler's feature root (sp_load_path_seed) also
       names the features the compiler's rule for a literal `require "x"`
       maps to a file (`bare`, newline-separated), the native libraries and
       the features loaded already.
   A candidate is
     - a file of the program (`top`): its absolute path without ".rb"; the
       program's Kernel#require or Kernel#require_relative (`require`, so the
       load has its frame) runs it the first time, answering true, and
       answers false after;
     - a native library (`loaded` only): its feature; its flag answers true
       the first time;
     - a feature loaded already (neither): its feature; false.
   Any other path raises CRuby's LoadError. Answers whether this call ran
   the file. */
typedef struct { const char *path; const char *bare; int *loaded; void (*top)(void); } sp_LoadCand;
int sp_require_search(const char *path, const sp_LoadCand *cands, int n, const char *dir, int rel,
                      int (*require)(const char *, int *, void (*)(void)));
/* What $LOAD_PATH holds as the program starts, under the graph model, as
   CRuby's does: the directories from -I, as File.expand_path gives them
   (against the directory the program was compiled in), in order, then the
   compiler's feature root (the place CRuby's own library directories take,
   so a directory the program appends comes after it and one it puts in front
   comes before). The strings are static; the array is made when the program
   first reads $LOAD_PATH or searches it. */
void sp_load_path_seed(const char *const *dirs, int n, const char *feature_root);
/* The LoadError of a require that loads nothing, as CRuby raises it: the
   message names the feature, and LoadError#path answers it. */
SP_NORETURN void sp_raise_load_error(const char *feature);

/* $LOAD_PATH (`$:`, `$-I`): an Array of Strings the program reads and
   changes. It starts empty (under the graph model, as sp_load_path_seed
   gives it): requires are resolved when the program is compiled, so there
   is no directory to look in as it runs, and a change the program makes to
   it looks nothing up, except for a path computed when the require runs
   (sp_require_search). Allocated and marked as
   $LOADED_FEATURES' array is. */
extern sp_StrArray *sp_load_path_cache;
sp_StrArray *sp_get_load_path(void);
int sp_argf_ensure(void);
const char *sp_argf_gets(void);
const char *sp_argf_read(void);
sp_StrArray *sp_argf_readlines(void);
const char *sp_argf_filename(void);
sp_bool sp_argf_eof(void);

#endif
