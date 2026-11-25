/* sp_rtclass.h -- classes made at run time (lib/sp_rtclass.c).
 *
 * A `Class.new` or `Module.new` the program evaluates more than once (in a
 * method, a block, a loop) makes a new class each time. Its code and layout
 * are one static class per call site (the site class); each evaluation adds
 * a record, whose id is the class value's cls_id. The ids sit past the
 * singleton-class range, below SP_CLASS_BY_NAME, so a class value of one
 * compares, hashes and boxes as any class value does, and only the readers
 * that name or dispatch on a class ask the record. Records are never freed.
 */
#ifndef SP_RTCLASS_H
#define SP_RTCLASS_H

#include "sp_types.h"

#define SP_RTCLASS_BASE (2 * SP_SCLASS_OFF)
#define SP_RTCLASS_CHUNK 1024
#define SP_RTCLASS_CHUNKS 1024
/* is class id `id` a record's? */
static inline int sp_rtclass_id_p(sp_int id) { return id >= SP_RTCLASS_BASE && id < SP_RTCLASS_BASE + SP_RTCLASS_CHUNK * SP_RTCLASS_CHUNKS; }

typedef struct {
  sp_int site;          /* the site class: the code, layout, constructor and
                           superclass, and whether it is a module */
  const char *name;     /* NULL until a constant names it */
  const char *key;      /* "#<Class:0x...>": its identity key, and its display until named */
  unsigned char frozen;
} sp_RtClass;

/* chunks of SP_RTCLASS_CHUNK records, never moved, so a reader takes no lock */
extern sp_RtClass *sp_rtclass_chunks[SP_RTCLASS_CHUNKS];

sp_Class sp_rtclass_new(sp_int site, int is_module);
/* how many records there are: their ids run from SP_RTCLASS_BASE, oldest
   first (Class#subclasses walks them) */
sp_int sp_rtclass_made(void);
static inline sp_RtClass *sp_rtclass_at(sp_int id) {
  sp_int n = id - SP_RTCLASS_BASE;
  return &sp_rtclass_chunks[n / SP_RTCLASS_CHUNK][n % SP_RTCLASS_CHUNK];
}
/* the id a switch over the program's classes takes: a record's site, any
   other id itself */
static inline sp_int sp_rtclass_site(sp_int id) { return sp_rtclass_id_p(id) ? sp_rtclass_at(id)->site : id; }
const char *sp_rtclass_display(sp_int id);
/* a record's frozen flag, for Class#freeze and #frozen? (sp_class_freeze_id):
   set by the first record made, so a program that makes none links none */
extern unsigned char *(*sp_rtclass_frozen_fn)(sp_int id);
void sp_rtclass_assign_name(sp_int id, const char *name);
/* The class made at run time a boxed instance of a site class was built
   for, -1 for any other value: the generated unit's, installed by a program
   that has a site class (sp_poly_class_val and the default renderings ask it) */
extern sp_int (*sp_obj_rtclass_fn)(sp_RbVal v);

#endif
