/* holder.h -- the variable a node names, and its C slot (#6765).

   A local, an instance variable, a global, a class variable and a
   constant each have their own way to find the slot behind a read or a
   write node: the scope's local, the ivar of the method's class (or the
   class body's, or the Toplevel's), the resolved global, the ancestor that
   owns the class variable, the constant table's entry. And each has its
   own C spelling for that slot: lv_<name> (or a cell), self->iv_<name> or
   civ_<Class>_<name>, gv_<name>, cvar_<Owner>_<name>, cst_<name>.

   holder_of_node answers both once: which holder a node names, as a
   HolderRef, and repr_of_holder its representation. holder_slot_buf
   spells its C slot. The emitters' arms do not all resolve an ivar or a
   class variable by the same rule yet (a class body's ivar, an
   instance_eval block, a multiple-assignment target); such an arm resolves
   the holder itself and builds the HolderRef with holder_ivar, or passes
   the class body it emits in to holder_of_node_in, and still spells the
   slot here. */
#ifndef SPINEL_HOLDER_H
#define SPINEL_HOLDER_H

#include "compiler.h"
#include "repr.h"

typedef enum {
  HK_NONE,
  HK_LOCAL,    /* a local or a parameter: lv */
  HK_IVAR,     /* cid, idx; cls_slot */
  HK_GVAR,     /* lv (the resolved global) */
  HK_CVAR,     /* cid (the owning ancestor), idx */
  HK_CONST     /* lv */
} HolderKind;

typedef struct {
  unsigned char kind;      /* HolderKind */
  unsigned char cls_slot;  /* HK_IVAR: the class-level C global
                              (civ_<Class>_<name>), not the object's field */
  int node;                /* the read, write or target node naming it */
  const char *name;        /* its Ruby name, as the node spells it */
  int cid, idx;            /* HK_IVAR / HK_CVAR: the class and its slot
                              index, -1 when the class has no entry */
  LocalVar *lv;            /* HK_LOCAL / HK_GVAR / HK_CONST: the slot */
  Repr r;                  /* repr_of_holder */
} HolderRef;

/* The holder kind a node of kind k names: a read, a write (=, ||=, &&=,
   op=) or a multiple-assignment target of a local, an ivar, a global, a
   class variable or a constant (bare or `A::B`, as a read or a target).
   HK_NONE for any other node. */
HolderKind holder_kind_of(NodeKind k);
/* Is k a read or a write (not a target) of a global, a constant or a class
   variable: a node whose value repr_of answers as its slot's? */
int holder_static_node(NodeKind k);

/* The class a node in the class body `body` (-1 for none) sees as its
   own: the method's class, else that body's, else the Toplevel. -1 when
   there is none. */
int holder_scope_class(const Compiler *c, int node, int body);
/* The class body the analysis put `node` in (node_cbody), or -1. */
int holder_cbody(const Compiler *c, int node);
/* The class a class variable `node` in the class body `body` looks its
   name up from, CRuby's cref: holder_scope_class's, except in a class
   builder's block and the methods defined there, whose cref is the class
   around the builder (elab_cvar_cref_at). -1 when there is no class (the
   top level before the Toplevel class is made). */
int holder_cvar_class(const Compiler *c, int node, int body);
/* ...for a node in a method or in the class body the analysis put it in
   (node_cbody); -1 for one in main's top-level code outside both, whose
   class variables the Toplevel holds, which a caller registers apart */
int holder_cvar_class_in_body(const Compiler *c, int node);
/* The class that holds class variable `nm` for `node` in the class body
   `body`: holder_cvar_class's, or the ancestor that declares it
   (comp_cvar_owner). -1 when there is no class. */
int holder_cvar_owner(const Compiler *c, int node, const char *nm, int body);

/* Resolve `node` (holder_kind_of) to its holder, with the class body the
   analysis put it in. 1 with *out filled, or 0: not a holder node, no
   such local / global / constant, no class for an ivar or a class
   variable. *out is defined either way: cleared (kind HK_NONE) for a node
   that names no holder and for an ivar with no class; its kind, node and
   name set when a local, global, constant or class variable has no slot.
   An ivar or class variable the class has no entry for resolves
   with idx -1 (its slot is still spelled). An ivar resolves by the read
   side's rule (strbuf_ivar_owner): the method's class, a class method's
   class-level global, a top-level method's Toplevel global, nothing
   inside an instance_eval block. */
int holder_of_node(const Compiler *c, int node, HolderRef *out);
int holder_of_node_slot(const Compiler *c, int node, HolderRef *out);   /* no Repr */
/* holder_of_node, in the class body `body` the caller says (codegen passes
   the body it is emitting, g_class_body_id); only a class variable's
   owner depends on it. */
int holder_of_node_in(const Compiler *c, int node, int body, HolderRef *out);
/* The ivar holder of `node` in class cid, by the caller's own storage
   rule: the class-level global when cls_slot, else the object's field. */
void holder_ivar(const Compiler *c, int node, int cid, int cls_slot, HolderRef *out);

/* The representation of h's slot: repr_of_slot for a local, a global or a
   constant, repr_of_ivar / repr_of_cvar for an ivar or a class variable
   (an empty Repr, TY_UNKNOWN, when its class has no entry). */
Repr repr_of_holder(const Compiler *c, const HolderRef *h);

/* (holder_slot_buf, h's C slot as an lvalue appended to a Buf, and
   holder_static_handle_buf, the same for a read or write node of a global,
   a constant or a class variable that holds the shared handle, are declared
   with Buf in codegen_internal.h) */

#endif
