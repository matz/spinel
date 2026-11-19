/* Canonical extension handles. The table is weak; arenas and registered
 * VALUE addresses provide the roots. Collection runs under the GC barrier.
 * Extension access will be serialized by the CXL in the threading stage. */
#ifdef SP_CEXT
#include "sp_cext.h"
#include "sp_alloc.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

typedef struct {
    sp_RbVal value;
    size_t pins;
    unsigned marked;
} sp_cext_handle;
static void handle_pin(sp_cext_handle *h) {
    SP_ATOMIC_FETCH_ADD(&h->pins, (size_t)1, __ATOMIC_RELAXED);
}
static void handle_unpin(sp_cext_handle *h) {
    SP_ATOMIC_FETCH_SUB(&h->pins, (size_t)1, __ATOMIC_RELAXED);
}
SP_TLS void *sp_cext_current_block;
SP_TLS VALUE sp_cext_errinfo = Qnil;
void sp_cext_set_errinfo(VALUE value) {
    if (!SPECIAL_CONST_P(value)) handle_pin((sp_cext_handle *)(uintptr_t)value);
    if (!SPECIAL_CONST_P(sp_cext_errinfo)) handle_unpin((sp_cext_handle *)(uintptr_t)sp_cext_errinfo);
    sp_cext_errinfo = value;
}
int (*sp_cext_data_class_fn)(int);
static sp_cext_handle **table;
static size_t capacity, count;
static SP_TLS sp_cext_handle **arena;
static SP_TLS size_t arena_n, arena_cap, arena_depth;
static VALUE **globals;
static size_t globals_n, globals_cap;
static void install_hooks(void) {
    sp_gc_mark_cext_hook = sp_cext_mark_roots;
    sp_gc_sweep_cext_hook = sp_cext_sweep_handles;
}

static void *allocate(size_t bytes) {
    void *p = malloc(bytes);
    if (!p) { fputs("spinel: C extension handle allocation failed\n", stderr); abort(); }
    return p;
}
static uint64_t payload(sp_RbVal v) {
    uint64_t bits = 0;
    if (v.tag == SP_TAG_FLT) memcpy(&bits, &v.v.f, sizeof(v.v.f));
    else if (v.tag == SP_TAG_INT || v.tag == SP_TAG_SYM ||
             (v.tag == SP_TAG_CLASS && v.cls_id != SP_CLASS_BY_NAME)) bits = (uint64_t)v.v.i;
    else bits = (uintptr_t)v.v.p;
    return bits;
}
static size_t hash(sp_RbVal v) {
    uint64_t x = payload(v) ^ ((uint64_t)(unsigned)v.tag << 32) ^ (unsigned)v.cls_id;
    x ^= x >> 30; x *= UINT64_C(0xbf58476d1ce4e5b9);
    x ^= x >> 27; x *= UINT64_C(0x94d049bb133111eb);
    return (size_t)(x ^ (x >> 31));
}
static size_t slot(sp_RbVal v) {
    size_t i = hash(v) & (capacity - 1);
    while (table[i] && (table[i]->value.tag != v.tag ||
           table[i]->value.cls_id != v.cls_id || payload(table[i]->value) != payload(v)))
        i = (i + 1) & (capacity - 1);
    return i;
}
static void rehash(size_t n) {
    sp_cext_handle **old = table;
    size_t old_cap = capacity;
    table = allocate(n * sizeof(*table)); memset(table, 0, n * sizeof(*table));
    capacity = n;
    for (size_t i = 0; i < old_cap; ++i)
        if (old[i]) table[slot(old[i]->value)] = old[i];
    free(old);
}
static void pin(sp_cext_handle *h) {
    if (!arena_depth) return;
    if (arena_n == arena_cap) {
        size_t n = arena_cap ? arena_cap * 2 : 32;
        sp_cext_handle **next = allocate(n * sizeof(*next));
        if (arena_n) memcpy(next, arena, arena_n * sizeof(*next));
        free(arena); arena = next; arena_cap = n;
    }
    arena[arena_n++] = h; handle_pin(h);
}
VALUE sp_cext_value(sp_RbVal v) {
    install_hooks();
    if (v.tag == SP_TAG_OBJ && v.v.p && v.cls_id >= 0 && sp_cext_data_class_fn && sp_cext_data_class_fn(v.cls_id))
        v.cls_id = (int)((sp_cext_data *)v.v.p)->cls_id;
    if (v.tag == SP_TAG_NIL) return Qnil;
    if (v.tag == SP_TAG_BOOL) return v.v.b ? Qtrue : Qfalse;
    if (v.tag == SP_TAG_INT && v.v.i >= RUBY_FIXNUM_MIN && v.v.i <= RUBY_FIXNUM_MAX)
        return LONG2FIX(v.v.i);
    if (v.tag == SP_TAG_SYM) return ID2SYM(v.v.i);
    if (!capacity) rehash(32);
    if ((count + 1) * 2 >= capacity) rehash(capacity * 2);
    size_t i = slot(v);
    if (!table[i]) {
        sp_cext_handle *h = allocate(sizeof(*h));
        h->value = v; h->pins = 0; h->marked = 0;
        table[i] = h; ++count;
    }
    pin(table[i]);
    return (VALUE)(uintptr_t)table[i];
}
sp_RbVal sp_cext_rbval(VALUE v) {
    sp_RbVal r = {0};
    if (v == Qnil) r.tag = SP_TAG_NIL;
    else if (v == Qtrue || v == Qfalse) { r.tag = SP_TAG_BOOL; r.v.b = v == Qtrue; }
    else if (FIXNUM_P(v)) { r.tag = SP_TAG_INT; r.v.i = FIX2LONG(v); }
    else if (SYMBOL_P(v)) { r.tag = SP_TAG_SYM; r.v.i = SYM2ID(v); }
    else if (v == Qundef) { fputs("spinel: Qundef is not a Ruby value\n", stderr); abort(); }
    else r = ((sp_cext_handle *)(uintptr_t)v)->value;
    return r;
}
sp_cext_arena_mark sp_cext_arena_enter(void) {
    install_hooks();
    sp_cext_arena_mark mark = {arena_n, arena_depth};
    ++arena_depth;
    return mark;
}
sp_cext_arena_mark sp_cext_arena_snapshot(void) {
    sp_cext_arena_mark mark = {arena_n, arena_depth}; return mark;
}
void sp_cext_arena_restore(sp_cext_arena_mark mark) {
    if (mark.depth > arena_depth || mark.handles > arena_n) abort();
    while (arena_n > mark.handles) handle_unpin(arena[--arena_n]);
    arena_depth = mark.depth;
}
size_t sp_cext_handle_count(void) { return count; }

void rb_gc_mark(VALUE v) {
    if (SPECIAL_CONST_P(v)) return;
    sp_cext_handle *h = (sp_cext_handle *)(uintptr_t)v;
    h->marked = sp_gc_mark_gen;
    sp_mark_rbval(h->value);
}
void rb_gc_mark_movable(VALUE v) { rb_gc_mark(v); }
VALUE rb_gc_location(VALUE v) { return v; }
int sp_cext_register_address(VALUE *address) {
    install_hooks();
    for (size_t i = 0; i < globals_n; ++i) if (globals[i] == address) return 0;
    if (globals_n == globals_cap) {
        size_t n = globals_cap ? globals_cap * 2 : 16;
        VALUE **next = allocate(n * sizeof(*next));
        if (globals_n) memcpy(next, globals, globals_n * sizeof(*next));
        free(globals); globals = next; globals_cap = n;
    }
    globals[globals_n++] = address;
    return 1;
}
void rb_gc_register_address(VALUE *address) { (void)sp_cext_register_address(address); }
void rb_global_variable(VALUE *address) { rb_gc_register_address(address); }
void rb_gc_unregister_address(VALUE *address) {
    for (size_t i = 0; i < globals_n; ++i) if (globals[i] == address) {
        globals[i] = globals[--globals_n]; return;
    }
}
void sp_cext_mark_roots(void) {
    /* Pins belong to active AND suspended fiber arenas. */
    for (size_t i = 0; i < capacity; ++i)
        if (table[i] && SP_ATOMIC_LOAD(&table[i]->pins, __ATOMIC_RELAXED)) rb_gc_mark((VALUE)(uintptr_t)table[i]);
    for (size_t i = 0; i < globals_n; ++i) rb_gc_mark(*globals[i]);
}
void sp_cext_arena_context_save(sp_cext_arena_context *ctx) {
    ctx->entries = arena; ctx->length = arena_n;
    ctx->capacity = arena_cap; ctx->depth = arena_depth;ctx->block=sp_cext_current_block;ctx->errinfo=sp_cext_errinfo;
}
void sp_cext_arena_context_load(const sp_cext_arena_context *ctx) {
    arena = ctx->entries; arena_n = ctx->length;
    arena_cap = ctx->capacity; arena_depth = ctx->depth;sp_cext_current_block=ctx->block;sp_cext_errinfo=ctx->errinfo?ctx->errinfo:Qnil;
}
void sp_cext_arena_context_dispose(sp_cext_arena_context *ctx) {
    sp_cext_handle **entries = ctx->entries;
    for (size_t i = 0; i < ctx->length; ++i) handle_unpin(entries[i]);
    if (ctx->errinfo && !SPECIAL_CONST_P(ctx->errinfo)) handle_unpin((sp_cext_handle *)(uintptr_t)ctx->errinfo);
    free(entries); memset(ctx, 0, sizeof(*ctx));
}
/* Called after the mark drains, before the sweep can recycle an address.
 * Canonical handles of live objects survive even without
 * a C root; the table itself must never keep those objects alive. */
static int live(sp_cext_handle *h, int full) {
    sp_RbVal v = h->value;
    if (SP_ATOMIC_LOAD(&h->pins, __ATOMIC_RELAXED) || (v.tag == SP_TAG_CLASS && v.cls_id != SP_CLASS_BY_NAME)) return 1;
    if ((v.tag == SP_TAG_OBJ && v.cls_id != SP_BUILTIN_FOREIGN_PTR &&
        v.cls_id != SP_BUILTIN_REGEX && v.cls_id != SP_BUILTIN_ARGF && v.cls_id != SP_BUILTIN_ENV) || v.tag == SP_TAG_BIGINT) {
        if (!v.v.p) return 0;
        const sp_gc_hdr *hdr = (const sp_gc_hdr *)v.v.p - 1;
        return hdr->marked == sp_gc_mark_gen || (!full && hdr->old);
    }
    if (v.tag == SP_TAG_CLASS && v.cls_id == SP_CLASS_BY_NAME) {
        const char *s = v.v.s;
        if (!s) return 0;
        unsigned char tag = (unsigned char)s[-1];
        if (tag == 0xff || tag == 0xfb || tag == 0xf1) return 1; /* immortal */
        const sp_str_hdr *view = ((const sp_str_hdr *)(s - 1)) - 1;
        if (tag == 0xf9) return !full || view->hash == (uint64_t)sp_gc_mark_gen;
        const void *owner;
        if (tag == 0xfd) {
            owner = view->next;
            if (!owner) return 1;
        } else if ((tag | 0x04) == 0xfe) {
            owner = SP_STR_BORN_LINK(view);
            if (!owner || (const char *)owner + sizeof(struct sp_String_s) != (const char *)view)
                return !full;
        } else {
            return !full;
        }
        const sp_gc_hdr *hdr = (const sp_gc_hdr *)owner - 1;
        return hdr->marked == sp_gc_mark_gen || (!full && hdr->old);
    }
    /* Heap liveness above is authoritative even on a GC generation wrap;
       an old handle stamp must never retain a pointer the sweep will free. */
    return h->marked == sp_gc_mark_gen;
}
void sp_cext_sweep_handles(int full) {
    int changed = 0;
    for (size_t i = 0; i < capacity; ++i) if (table[i]) {
        if (!live(table[i], full)) { free(table[i]); table[i] = NULL; --count; changed = 1; }
        else table[i]->marked = 0;
    }
    if (changed) rehash(capacity);
}

void sp_cext_data_scan(void *object) {
    sp_cext_data *wrapper = object;
    if (wrapper->cext_data && wrapper->cext_type && wrapper->cext_type->function.dmark)
        wrapper->cext_type->function.dmark(wrapper->cext_data);
}
void sp_cext_data_finalize(void *object) {
    sp_cext_data *wrapper = object;
    if (!wrapper->cext_data || !wrapper->cext_type) return;
    RUBY_DATA_FUNC fn = wrapper->cext_type->function.dfree;
    if (fn == RUBY_TYPED_DEFAULT_FREE) free(wrapper->cext_data);
    else if (fn) fn(wrapper->cext_data);
    wrapper->cext_data = NULL;
}
sp_cext_data *sp_cext_data_new(int cls_id, void *data, const rb_data_type_t *type) {
    /* No GC allocation is permitted between allocation and publication into
       the caller's arena/root. GC allocations may collect before allocating. */
    sp_cext_data *wrapper = sp_gc_alloc(sizeof(*wrapper), sp_cext_data_finalize, sp_cext_data_scan);
    wrapper->cls_id = cls_id; wrapper->cext_data = data; wrapper->cext_type = type;
    if (!type || !(type->flags & RUBY_TYPED_WB_PROTECTED)) sp_gc_pin_remembered(wrapper);
    return wrapper;
}
int sp_cext_data_is_kind_of(const sp_cext_data *object, const rb_data_type_t *type) {
    for (const rb_data_type_t *t = object->cext_type; t; t = t->parent)
        if (t == type) return 1;
    return 0;
}
#endif /* SP_CEXT: wildcard runtime/export builds compile an empty TU. */
