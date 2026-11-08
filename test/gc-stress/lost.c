/* gc-stress-test: a host that loses an object on purpose.
 *
 * `lost` and `text` live in C locals the collector is never told about, and
 * the allocation after them collects when SPINEL_GC_STRESS=2 is set. Without
 * it nothing collects in a program this small, and both read back what was
 * written: the wrong program answers right. With it the sweep frees both,
 * poisons them and keeps the slots out of reuse, so the reads see 0xdb (the
 * text only by its first byte: what follows a freed String depends on the
 * heap's layout), and
 * rooting `lost` afterwards hands the next collection's mark a freed slot,
 * which it reports and aborts on. See "A lost object, made visible" in
 * docs/internals/gc.md. */
#include <stdio.h>
#include <string.h>
#include "sp_alloc.h"

typedef struct { long v; } box;

int main(void) {
  box *kept = (box *)sp_gc_alloc(sizeof(box), NULL, NULL);
  SP_GC_ROOT(kept);
  kept->v = 7;
  box *lost = (box *)sp_gc_alloc(sizeof(box), NULL, NULL);
  lost->v = 7;
  char *text = sp_str_alloc(4);
  memcpy(text, "text", 4);
  (void)sp_gc_alloc(sizeof(box), NULL, NULL);
  printf("kept %02x\n", *(unsigned char *)&kept->v);
  printf("lost %02x\n", *(unsigned char *)&lost->v);
  printf("text %02x\n", (unsigned char)text[0]);
  fflush(stdout);
  SP_GC_ROOT(lost);
  (void)sp_gc_alloc(sizeof(box), NULL, NULL);
  puts("the mark did not stop");
  return 0;
}
