/* Exception bridge over the generated translation unit's handler stack. */
#ifdef SP_CEXT
#include "sp_cext.h"
#include "sp_exc.h"
#include "sp_setjmp.h"
#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

void sp_exc_arm(jmp_buf);
void sp_exc_disarm(void);
const char *sp_exc_cur_cls(void);
const char *sp_exc_cur_msg(void);
void *sp_exc_cur_obj(void);
void sp_fiber_reraise(const char *, const char *, void *);
int sp_cext_handled_push(void *);
void sp_cext_handled_restore(int);
int sp_cext_exception_p(sp_RbVal);
sp_RbVal sp_cext_box_exception(void *);
void sp_cext_exception_caught(void *);

VALUE rb_eException, rb_eStandardError, rb_eRuntimeError, rb_eTypeError;
VALUE rb_eArgError, rb_eRangeError, rb_eNoMemError, rb_eSystemCallError;
VALUE rb_eNotImpError;
#define errinfo sp_cext_errinfo
static int initialized;
sp_RbVal (*sp_cext_exception_new_fn)(const char *, const char *) = NULL;

static VALUE named_class(const char *name) {
    sp_RbVal v = {SP_TAG_CLASS, SP_CLASS_BY_NAME, {.s = name}};
    return sp_cext_value(v);
}
void sp_cext_exceptions_init(void) {
    if (initialized) return;
    initialized = 1;
    rb_eException = named_class(&("\xff" "Exception")[1]);
    rb_eStandardError = named_class(&("\xff" "StandardError")[1]);
    rb_eRuntimeError = named_class(&("\xff" "RuntimeError")[1]);
    rb_eTypeError = named_class(&("\xff" "TypeError")[1]);
    rb_eArgError = named_class(&("\xff" "ArgumentError")[1]);
    rb_eRangeError = named_class(&("\xff" "RangeError")[1]);
    rb_eNoMemError = named_class(&("\xff" "NoMemoryError")[1]);
    rb_eSystemCallError = named_class(&("\xff" "SystemCallError")[1]);
    rb_eNotImpError = named_class(&("\xff" "NotImplementedError")[1]);
}
static const char *class_name(VALUE klass) {
    sp_RbVal v = sp_cext_rbval(klass);
    if (v.tag == SP_TAG_CLASS) {
        if (v.cls_id == SP_CLASS_BY_NAME) return v.v.s;
        if (sp_obj_cls_name_fn) return sp_obj_cls_name_fn((int)v.v.i);
    }
    sp_raise_cls("TypeError", "exception class required");
}
static sp_Exception *exception(VALUE value) {
    sp_RbVal v = sp_cext_rbval(value);
    if (sp_cext_exception_p(v)) return v.v.p;
    sp_raise_cls("TypeError", "exception object required");
}
static VALUE box_exception(sp_Exception *e) {
    return sp_cext_value(sp_cext_box_exception(e));
}
static VALUE new_exception(const char *name, const char *message) {
    if (sp_user_exc_parent_fn && sp_user_exc_parent_fn(name)) {
        if (!sp_cext_exception_new_fn)
            sp_raise_cls("NotImplementedError", "C extension subclass allocator is not installed");
        return sp_cext_value(sp_cext_exception_new_fn(name, message));
    }
    return box_exception(sp_exc_new_for_catch(name, message));
}
VALUE rb_exc_new(VALUE klass, const char *message, long length) {
    if (length < 0) sp_raise_cls("ArgumentError", "negative string size");
    const char *name = class_name(klass);
    char *copy = sp_str_alloc((size_t)length);
    if (length) memcpy(copy, message, (size_t)length);
    SP_GC_ROOT_STR(copy);
    return new_exception(name, copy);
}
VALUE rb_exc_new_cstr(VALUE klass, const char *message) {
    return rb_exc_new(klass, message, (long)strlen(message));
}
const char *sp_poly_to_s(sp_RbVal);
const char *sp_poly_inspect(sp_RbVal);
/* printf's conversions, plus CRuby's "%"PRIsVALUE: an integer conversion
 * followed by \v takes a VALUE and prints its to_s, or its inspect with the
 * + flag. Each conversion is formatted on its own, so the argument list is
 * read by the conversion's own type. */
static void append(char **out, size_t *length, size_t *capacity, const char *text, size_t n) {
    if (*length + n + 1 > *capacity) {
        size_t next = (*length + n + 1) * 2;
        char *grown = realloc(*out, next);
        if (!grown) { free(*out); sp_raise_cls("NoMemoryError", "exception format allocation failed"); }
        *out = grown; *capacity = next;
    }
    memcpy(*out + *length, text, n); *length += n; (*out)[*length] = 0;
}
static const char *format(const char *fmt, va_list args) {
    char *out = NULL; size_t length = 0, capacity = 0;
    append(&out, &length, &capacity, "", 0);
    for (const char *p = fmt; *p;) {
        if (*p != '%') { const char *q = strchr(p, '%'); size_t n = q ? (size_t)(q - p) : strlen(p); append(&out, &length, &capacity, p, n); p += n; continue; }
        if (p[1] == '%') { append(&out, &length, &capacity, "%", 1); p += 2; continue; }
        const char *start = p++;
        int plus = 0;
        while (*p && strchr("-+ #0", *p)) { if (*p == '+') plus = 1; p++; }
        int star = 0;
        if (*p == '*') { star++; p++; } else while (*p >= '0' && *p <= '9') p++;
        if (*p == '.') { p++; if (*p == '*') { star++; p++; } else while (*p >= '0' && *p <= '9') p++; }
        int longs = 0, size_t_arg = 0, short_arg = 0;
        for (;; p++) {
            if (*p == 'l') longs++;
            else if (*p == 'z' || *p == 't' || *p == 'j') size_t_arg = 1;
            else if (*p == 'h') short_arg = 1;
            else if (*p == 'L' || *p == 'q') longs = 2;
            else break;
        }
        char conversion = *p;
        if (!conversion) sp_raise_cls("ArgumentError", "invalid exception format");
        p++;
        char spec[64]; size_t spec_length = (size_t)(p - start);
        if (spec_length >= sizeof spec) sp_raise_cls("ArgumentError", "invalid exception format");
        memcpy(spec, start, spec_length); spec[spec_length] = 0;
        int widths[2] = {0, 0};
        for (int i = 0; i < star; i++) widths[i] = va_arg(args, int);
        if ((conversion == 'i' || conversion == 'd') && *p == '\v') {
            p++;
            VALUE value = longs ? (VALUE)va_arg(args, VALUE) : (VALUE)va_arg(args, int);
            const char *text = plus ? sp_poly_inspect(sp_cext_rbval(value)) : sp_poly_to_s(sp_cext_rbval(value));
            append(&out, &length, &capacity, text, strlen(text));
            continue;
        }
        char piece[512]; int n;
#define PIECE(arg) (star == 2 ? snprintf(piece, sizeof piece, spec, widths[0], widths[1], arg) : \
                    star == 1 ? snprintf(piece, sizeof piece, spec, widths[0], arg) : snprintf(piece, sizeof piece, spec, arg))
        switch (conversion) {
        case 'd': case 'i': case 'u': case 'x': case 'X': case 'o': case 'c':
            if (size_t_arg) n = PIECE(va_arg(args, size_t));
            else if (longs >= 2) n = PIECE(va_arg(args, long long));
            else if (longs == 1) n = PIECE(va_arg(args, long));
            else { (void)short_arg; n = PIECE(va_arg(args, int)); }
            break;
        case 'e': case 'E': case 'f': case 'F': case 'g': case 'G': case 'a': case 'A':
            n = longs >= 2 ? PIECE(va_arg(args, long double)) : PIECE(va_arg(args, double)); break;
        case 's': {
            const char *text = va_arg(args, const char *);
            n = PIECE(text ? text : "(null)");
            if (n >= (int)sizeof piece && !star && spec_length == 2) { append(&out, &length, &capacity, text, strlen(text)); continue; }
            break;
        }
        case 'p': n = PIECE(va_arg(args, void *)); break;
        default: sp_raise_cls("ArgumentError", "invalid exception format");
        }
#undef PIECE
        if (n < 0) sp_raise_cls("ArgumentError", "invalid exception format");
        if (n >= (int)sizeof piece) n = (int)sizeof piece - 1;
        append(&out, &length, &capacity, piece, (size_t)n);
    }
    char *s = sp_str_alloc(length);
    memcpy(s, out, length + 1); free(out);
    return s;
}
void rb_raise(VALUE klass, const char *fmt, ...) {
    const char *name = class_name(klass);
    va_list args; va_start(args, fmt);
    const char *message = format(fmt, args); va_end(args);
    SP_GC_ROOT_STR(message);
    rb_exc_raise(new_exception(name, message));
}
void rb_exc_raise(VALUE value) {
    sp_Exception *e = exception(value);
    sp_cext_set_errinfo(value);
    sp_fiber_reraise(e->cls_name, sp_exc_raise_msg(e), e);
    abort();
}
VALUE rb_errinfo(void) { return errinfo; }
void rb_set_errinfo(VALUE value) {
    if (!NIL_P(value)) (void)exception(value);
    sp_cext_set_errinfo(value);
}
VALUE rb_protect(VALUE (*body)(VALUE), VALUE argument, int *state) {
    sp_cext_exceptions_init();
    const int roots = sp_gc_nroots;
    sp_cext_arena_mark checkpoint = sp_cext_arena_snapshot();
    jmp_buf frame;
    if (setjmp(frame) == 0) {
        sp_exc_arm(frame);
        VALUE result = body(argument);
        sp_RbVal returned = sp_cext_rbval(result);
        sp_exc_disarm();
        sp_cext_arena_restore(checkpoint);
        sp_gc_nroots = roots;
        if (state) *state = 0;
        return sp_cext_value(returned);
    }
    const char *cls = sp_exc_cur_cls(), *message = sp_exc_cur_msg();
    sp_Exception *object = sp_exc_cur_obj();
    sp_exc_disarm();
    sp_cext_arena_restore(checkpoint);
    sp_gc_nroots = roots;
    SP_GC_ROOT_STR(message);
    SP_GC_ROOT(object);
    if (!object) object = sp_cext_rbval(new_exception(cls, message)).v.p;
    sp_cext_exception_caught(object);
    sp_cext_set_errinfo(box_exception(object));
    if (state) *state = !strcmp(cls, "fatal") ? 8 : 6; /* TAG_FATAL / TAG_RAISE */
    return Qnil;
}
void rb_jump_tag(int state) { if (state) rb_exc_raise(errinfo); }
VALUE rb_ensure(VALUE (*body)(VALUE), VALUE arg, VALUE (*ensure)(VALUE), VALUE ensure_arg) {
    int state;
    VALUE result = rb_protect(body, arg, &state);
    VALUE raised = errinfo;
    sp_RbVal saved = sp_cext_rbval(raised), returned = sp_cext_rbval(result);
    SP_GC_ROOT_RBVAL(saved); SP_GC_ROOT_RBVAL(returned);
    int handled = state ? sp_cext_handled_push(exception(raised)) : -1;
    ensure(ensure_arg); /* An exception here supersedes the body's exception. */
    if (handled >= 0) sp_cext_handled_restore(handled);
    if (state) rb_exc_raise(sp_cext_value(saved));
    return sp_cext_value(returned);
}
static VALUE rescue(VALUE (*body)(VALUE), VALUE arg, VALUE (*handler)(VALUE, VALUE),
                    VALUE handler_arg, VALUE klass, va_list *classes) {
    sp_cext_exceptions_init();
    sp_RbVal prior = sp_cext_rbval(errinfo); SP_GC_ROOT_RBVAL(prior);
    int state;
    VALUE result = rb_protect(body, arg, &state);
    if (!state) { if (classes) va_end(*classes); return result; }
    sp_Exception *e = exception(errinfo);
    int matches = 0;
    if (classes) {
        for (VALUE k; (k = va_arg(*classes, VALUE)) != 0;)
            if (state != 8 && sp_exc_is_a(e, class_name(k))) matches = 1;
        va_end(*classes);
    }
    else matches = state != 8 && sp_exc_is_a(e, class_name(klass));
    if (matches) {
        int handled = sp_cext_handled_push(e);
        result = handler ? handler(handler_arg, errinfo) : Qnil;
        sp_cext_handled_restore(handled);
        sp_cext_set_errinfo(sp_cext_value(prior));
        return result;
    }
    rb_jump_tag(state);
    abort();
}
VALUE rb_rescue(VALUE (*body)(VALUE), VALUE arg, VALUE (*handler)(VALUE, VALUE), VALUE handler_arg) {
    sp_cext_exceptions_init();
    return rescue(body, arg, handler, handler_arg, rb_eStandardError, NULL);
}
VALUE rb_rescue2(VALUE (*body)(VALUE), VALUE arg, VALUE (*handler)(VALUE, VALUE), VALUE handler_arg, ...) {
    va_list classes; va_start(classes, handler_arg);
    return rescue(body, arg, handler, handler_arg, Qnil, &classes);
}

void rb_sys_fail(const char *message) {
    int error = errno;
    const char *cls = sp_errno_class_name(error);
    sp_raise_cls(cls, message ? sp_sprintf("%s - %s", strerror(error), message) : strerror(error));
}
static void fatal_message(const char *prefix, const char *fmt, va_list args) {
    fputs(prefix, stderr); vfprintf(stderr, fmt, args); fputc('\n', stderr); abort();
}
void rb_bug(const char *fmt, ...) {
    va_list args; va_start(args, fmt); fatal_message("[BUG] ", fmt, args); abort();
}
void rb_fatal(const char *fmt, ...) {
    va_list args; va_start(args, fmt);
    const char *message = format(fmt, args); va_end(args);
    sp_raise_cls("fatal", message);
}
#endif /* SP_CEXT: do not export rb_* symbols into a host CRuby extension. */
