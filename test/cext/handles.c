#include "sp_cext.h"
#include "sp_string.h"
#include "sp_str.h"
#include <assert.h>
#include <stdio.h>

void sp_raise_cls(const char *cls, const char *message) {
    fprintf(stderr, "%s: %s\n", cls, message); abort();
}
static int finalized;
static void finalize(void *p) { (void)p; ++finalized; }
typedef struct { VALUE child; } payload;
static int data_freed, data_marked;
static void mark_data(void *p) { ++data_marked; rb_gc_mark(((payload *)p)->child); }
static void free_data(void *p) { ++data_freed; free(p); }
static const rb_data_type_t parent_type = {.wrap_struct_name = "Parent"};
static const rb_data_type_t data_type = {
    .wrap_struct_name = "Child", .function = {.dmark = mark_data, .dfree = free_data},
    .parent = &parent_type
};
int main(void) {
    void *early = sp_gc_alloc(sizeof(long), NULL, NULL);
    sp_RbVal early_value = {SP_TAG_OBJ, 123, {.p = early}};
    (void)sp_cext_value(early_value); /* no arena: must still install weak-table hooks */
    sp_gc_collect();
    assert(sp_cext_handle_count() == 0);
    VALUE registered = Qnil;
    rb_gc_register_address(&registered);
    early = sp_gc_alloc(sizeof(long), NULL, NULL); early_value.v.p = early;
    registered = sp_cext_value(early_value);
    sp_gc_collect();
    assert(sp_cext_handle_count() == 1 && sp_cext_rbval(registered).v.p == early);
    rb_gc_unregister_address(&registered);
    for (int i = 0; i < 256 && sp_cext_handle_count(); ++i) sp_gc_collect();
    assert(sp_cext_handle_count() == 0);
    /* A class name large enough to use malloc instead of a slab slot. */
    {
        char *bytes = sp_str_alloc(4096);
        memset(bytes, 'a', 4096);
        const char *name = sp_str_retained(bytes);
        SP_GC_ROOT_STR(name);
        sp_RbVal cls = {SP_TAG_CLASS, SP_CLASS_BY_NAME, {.s = name}};
        (void)sp_cext_value(cls);
        sp_gc_full_next = 1;
        sp_gc_collect();
        assert(sp_cext_handle_count() == 1);
    }
    sp_gc_full_next = 1;
    sp_gc_collect();
    assert(sp_cext_handle_count() == 0);
    /* Retiring an outside payload leaves a rooted byte view readable. Its
       weak handle must still be canonical, and disappear with that view. */
    {
        char *payload = malloc(SP_FD_OVH + 4);
        assert(payload);
        char *bytes = sp_fd_setup(payload);
        memcpy(bytes, "Name", 5);
        const char *name = bytes;
        SP_GC_ROOT_STR(name);
        sp_RbVal cls = {SP_TAG_CLASS, SP_CLASS_BY_NAME, {.s = name}};
        VALUE canonical = sp_cext_value(cls);
        sp_fd_retire(payload);
        char *binary_payload = malloc(SP_FD_OVH + 3);
        assert(binary_payload);
        const char *binary = sp_fd_setup(binary_payload);
        SP_GC_ROOT_STR(binary);
        memcpy((char *)binary, "\x80\0b", 4);
        sp_str_set_len((char *)binary, 3);
        sp_str_mark_binary((char *)binary);
        sp_fd_retire(binary_payload);
        sp_gc_full_next = 1;
        sp_gc_collect();
        assert(sp_cext_handle_count() == 1 && sp_cext_value(cls) == canonical);
        assert(strcmp(name, "Name") == 0);
        assert(sp_str_byte_len(binary) == 3 && sp_str_is_binary(binary));
        char *copy = (char *)sp_str_from_bytes("\x80\0b", 3);
        SP_GC_ROOT_STR(copy);
        sp_str_mark_binary(copy);
        const sp_str_hdr *retired = (const sp_str_hdr *)binary_payload;
        uint64_t generation = retired->hash;
        assert(sp_str_probe_eq(binary, copy) && sp_str_hash(binary) == sp_str_hash(copy));
        assert(retired->hash == generation);
    }
    sp_gc_full_next = 1;
    sp_gc_collect();
    assert(sp_cext_handle_count() == 0);
    sp_cext_arena_mark outer = sp_cext_arena_enter();
    long *object = sp_gc_alloc(sizeof(long), finalize, NULL);
    *object = 42;
    sp_RbVal box = {SP_TAG_OBJ, 123, {.p = object}};
    VALUE a = sp_cext_value(box), b = sp_cext_value(box);
    assert(a == b && sp_cext_rbval(a).v.p == object);
    sp_cext_arena_mark initial_inner = sp_cext_arena_enter();
    void *dead = sp_gc_alloc(sizeof(long), NULL, NULL);
    sp_RbVal dead_value = {SP_TAG_OBJ, 123, {.p = dead}};
    (void)sp_cext_value(dead_value);
    sp_gc_collect();
    assert(!finalized && *object == 42);
    sp_cext_arena_restore(initial_inner);
    sp_gc_mark_gen = 0x7ffffffu;
    sp_gc_cycle = 0; /* Make the rollover collection full. */
    sp_gc_collect();
    assert(sp_gc_mark_gen == 1 && sp_cext_handle_count() == 1);
    sp_cext_arena_mark inner = sp_cext_arena_enter();
    sp_RbVal number = {SP_TAG_FLT, 0, {.f = 1.5}};
    VALUE f = sp_cext_value(number);
    assert(f == sp_cext_value(number));
    assert(sp_cext_rbval(f).v.f == 1.5);
    sp_cext_arena_restore(inner);
    rb_gc_register_address(&a);
    rb_gc_register_address(&a);
    sp_cext_arena_restore(outer);
    sp_gc_collect();
    assert(!finalized && sp_cext_value(box) == a);
    assert(sp_cext_handle_count() == 1);
    rb_gc_unregister_address(&a);
    for (int i = 0; i < 256 && !finalized; ++i) sp_gc_collect();
    assert(finalized == 1 && sp_cext_handle_count() == 0);
    /* A Ruby root also preserves canonical identity without pinning the
       entire weak handle table. */
    outer = sp_cext_arena_enter();
    object = sp_gc_alloc(sizeof(long), finalize, NULL);
    SP_GC_ROOT(object);
    box.v.p = object;
    a = sp_cext_value(box);
    sp_cext_arena_restore(outer);
    sp_gc_collect();
    assert(sp_cext_value(box) == a);
    assert(rb_gc_location(a) == a);
    outer = sp_cext_arena_enter();
    payload *data = malloc(sizeof(*data));
    data->child = Qnil;
    sp_cext_data *wrapper = sp_cext_data_new(124, data, &data_type);
    sp_RbVal wrapped = {SP_TAG_OBJ, 124, {.p = wrapper}};
    VALUE w = sp_cext_value(wrapped);
    rb_global_variable(&w);
    assert(sp_cext_data_is_kind_of(wrapper, &parent_type));
    sp_cext_arena_restore(outer);
    sp_gc_collect();
    outer = sp_cext_arena_enter();
    long *child = sp_gc_alloc(sizeof(long), finalize, NULL);
    *child = 77;
    sp_RbVal child_value = {SP_TAG_OBJ, 123, {.p = child}};
    data->child = sp_cext_value(child_value); /* no write barrier */
    sp_cext_arena_restore(outer);
    int before = finalized;
    sp_gc_collect();
    assert(data_marked && !data_freed && finalized == before && *child == 77);
    assert(sp_cext_rbval(data->child).v.p == child);
    rb_gc_unregister_address(&w);
    for (int i = 0; i < 256 && !data_freed; ++i) sp_gc_collect();
    assert(data_freed == 1 && finalized == before + 1);
    outer = sp_cext_arena_enter();
    sp_RbVal string = sp_box_obj(sp_String_from_cstr("text"), SP_BUILTIN_STRBUF);
    VALUE s = sp_cext_value(string);
    assert(s == sp_cext_value(string));
    rb_global_variable(&s);
    sp_cext_arena_restore(outer);
    sp_gc_collect();
    assert(strcmp(sp_rbval_string_bytes(sp_cext_rbval(s)), "text") == 0);
    assert(sp_cext_value(string) == s);
    rb_gc_unregister_address(&s);
    size_t handles_before = sp_cext_handle_count();
    sp_gc_full_next = 1;
    sp_gc_collect();
    assert(sp_cext_handle_count() < handles_before);
    /* Unwind several C calls at once, including an empty inner arena. */
    outer = sp_cext_arena_enter();
    (void)sp_cext_arena_enter();
    (void)sp_cext_arena_enter();
    (void)sp_cext_value(number);
    sp_cext_arena_restore(outer);
    sp_gc_collect();
    assert(sp_cext_arena_snapshot().depth == 0);
    outer = sp_cext_arena_enter();
    VALUE many[200];
    for (int i = 0; i < 200; ++i) {
        long *p = sp_gc_alloc(sizeof(long), NULL, NULL);
        *p = i;
        sp_RbVal v = {SP_TAG_OBJ, 123, {.p = p}};
        many[i] = sp_cext_value(v);
    }
    sp_gc_collect();
    for (int i = 0; i < 200; ++i) {
        sp_RbVal v = sp_cext_rbval(many[i]);
        assert(*(long *)v.v.p == i && sp_cext_value(v) == many[i]);
    }
    sp_cext_arena_restore(outer);
    for (int i = 0; i < 256; ++i) sp_gc_collect();
    assert(sp_cext_handle_count() == 1); /* the Ruby-rooted object above */
    puts("cext-gc-test: pass");
}
