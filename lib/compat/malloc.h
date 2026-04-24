/* macOS shim: <malloc.h> does not exist on Darwin — malloc/free/realloc
   live in <stdlib.h>. This shim lets sources that unconditionally include
   <malloc.h> compile on macOS when lib/compat is on the include path.

   malloc_trim() is a glibc extension with no macOS equivalent (Darwin's
   allocator reclaims on its own). Stub it to a no-op so call sites link. */
#ifndef SP_COMPAT_MALLOC_H
#define SP_COMPAT_MALLOC_H
#include <stddef.h>
#include <stdlib.h>
static inline int malloc_trim(size_t pad) { (void)pad; return 0; }
#endif
