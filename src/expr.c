/* expr.c - Spinel AOT: expression code generation */
#define _GNU_SOURCE  /* for open_memstream */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdbool.h>
#include "codegen.h"

/* ------------------------------------------------------------------ */
/* Helper: extract block parameter name from a block node             */
/* Handles explicit |x|, numbered _1, and it-block parameters         */
/* ------------------------------------------------------------------ */
static char *extract_block_param(codegen_ctx_t *ctx, pm_block_node_t *blk) {
    if (blk->parameters) {
        if (PM_NODE_TYPE(blk->parameters) == PM_BLOCK_PARAMETERS_NODE) {
            pm_block_parameters_node_t *bp = (pm_block_parameters_node_t *)blk->parameters;
            if (bp->parameters && bp->parameters->requireds.size > 0) {
                pm_node_t *p = bp->parameters->requireds.nodes[0];
                if (PM_NODE_TYPE(p) == PM_REQUIRED_PARAMETER_NODE)
                    return cstr(ctx, ((pm_required_parameter_node_t *)p)->name);
            }
        }
        if (PM_NODE_TYPE(blk->parameters) == PM_NUMBERED_PARAMETERS_NODE) {
            return xstrdup("_1");
        }
        if (PM_NODE_TYPE(blk->parameters) == PM_IT_PARAMETERS_NODE) {
            return xstrdup("_1");
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Expression codegen                                                 */
/* ------------------------------------------------------------------ */


char *codegen_expr(codegen_ctx_t *ctx, pm_node_t *node) {
    if (!node) return xstrdup("/* nil */");

    switch (PM_NODE_TYPE(node)) {
    case PM_INTEGER_NODE: {
        pm_integer_node_t *n = (pm_integer_node_t *)node;
        if (n->value.length == 0) {
            int64_t val = (int64_t)n->value.value;
            if (n->value.negative) val = -val;
            return sfmt("%lld", (long long)val);
        }
        return xstrdup("0");
    }

    case PM_FLOAT_NODE: {
        pm_float_node_t *n = (pm_float_node_t *)node;
        return sfmt("%.17g", n->value);
    }

    case PM_STRING_NODE: {
        pm_string_node_t *n = (pm_string_node_t *)node;
        const uint8_t *src = pm_string_source(&n->unescaped);
        size_t len = pm_string_length(&n->unescaped);
        size_t bufsz = len * 4 + 64;
        char *buf = malloc(bufsz);
        int pos = snprintf(buf, bufsz, "\"");
        for (size_t i = 0; i < len; i++) {
            uint8_t c = src[i];
            if (c == '"') pos += snprintf(buf + pos, bufsz - pos, "\\\"");
            else if (c == '\\') pos += snprintf(buf + pos, bufsz - pos, "\\\\");
            else if (c == '\n') pos += snprintf(buf + pos, bufsz - pos, "\\n");
            else if (c == '\r') pos += snprintf(buf + pos, bufsz - pos, "\\r");
            else if (c == '\t') pos += snprintf(buf + pos, bufsz - pos, "\\t");
            else if (c == '%') pos += snprintf(buf + pos, bufsz - pos, "%%");
            else if (c >= 32 && c < 127) pos += snprintf(buf + pos, bufsz - pos, "%c", c);
            else pos += snprintf(buf + pos, bufsz - pos, "\\x%02x", c);
        }
        snprintf(buf + pos, bufsz - pos, "\"");
        return buf;
    }

    case PM_INTERPOLATED_STRING_NODE: {
        pm_interpolated_string_node_t *n = (pm_interpolated_string_node_t *)node;
        /* AOT path: build string using sp_str_concat / sp_int_to_s / sp_poly_to_s */
        int id = ctx->temp_counter++;
        emit(ctx, "const char *_is%d = \"\";\n", id);
        for (size_t i = 0; i < n->parts.size; i++) {
            pm_node_t *part = n->parts.nodes[i];
            if (PM_NODE_TYPE(part) == PM_STRING_NODE) {
                char *s = codegen_expr(ctx, part);
                emit(ctx, "_is%d = sp_str_concat(_is%d, %s);\n", id, id, s);
                free(s);
            } else if (PM_NODE_TYPE(part) == PM_EMBEDDED_STATEMENTS_NODE) {
                pm_embedded_statements_node_t *e = (pm_embedded_statements_node_t *)part;
                if (e->statements && e->statements->body.size > 0) {
                    char *ie = codegen_expr(ctx, e->statements->body.nodes[0]);
                    vtype_t it = infer_type(ctx, e->statements->body.nodes[0]);
                    if (it.kind == SPINEL_TYPE_INTEGER)
                        emit(ctx, "_is%d = sp_str_concat(_is%d, sp_int_to_s(%s));\n", id, id, ie);
                    else if (it.kind == SPINEL_TYPE_FLOAT)
                        emit(ctx, "_is%d = sp_str_concat(_is%d, sp_float_to_s(%s));\n", id, id, ie);
                    else if (it.kind == SPINEL_TYPE_POLY)
                        emit(ctx, "_is%d = sp_str_concat(_is%d, sp_poly_to_s(%s));\n", id, id, ie);
                    else if (it.kind == SPINEL_TYPE_BOOLEAN)
                        emit(ctx, "_is%d = sp_str_concat(_is%d, %s ? \"true\" : \"false\");\n", id, id, ie);
                    else if (it.kind == SPINEL_TYPE_NIL)
                        emit(ctx, "_is%d = sp_str_concat(_is%d, \"\");\n", id, id);
                    else
                        emit(ctx, "_is%d = sp_str_concat(_is%d, %s);\n", id, id, ie);
                    free(ie);
                }
            }
        }
        return sfmt("_is%d", id);
    }

    case PM_TRUE_NODE:  return xstrdup("TRUE");
    case PM_FALSE_NODE: return xstrdup("FALSE");
    case PM_NIL_NODE:   return xstrdup("0 /* nil */");

    case PM_SOURCE_LINE_NODE: {
        pm_source_line_node_t *n = (pm_source_line_node_t *)node;
        /* Calculate line number from location offset */
        pm_line_column_t lc = pm_newline_list_line_column(&ctx->parser->newline_list, n->base.location.start, ctx->parser->start_line);
        return sfmt("%d", (int)lc.line);
    }

    case PM_SOURCE_FILE_NODE:
        return sfmt("\"%s\"", ctx->parser->filepath.source ? (const char *)ctx->parser->filepath.source : "unknown");

    case PM_DEFINED_NODE: {
        pm_defined_node_t *n = (pm_defined_node_t *)node;
        /* For local variables: check if defined in var table */
        if (PM_NODE_TYPE(n->value) == PM_LOCAL_VARIABLE_READ_NODE) {
            pm_local_variable_read_node_t *lv = (pm_local_variable_read_node_t *)n->value;
            char *name = cstr(ctx, lv->name);
            var_entry_t *v = var_lookup(ctx, name);
            free(name);
            return xstrdup(v ? "\"local-variable\"" : "NULL");
        }
        /* For methods, constants, etc. — conservatively return non-NULL */
        return xstrdup("\"expression\"");
    }

    case PM_SYMBOL_NODE: {
        pm_symbol_node_t *n = (pm_symbol_node_t *)node;
        const uint8_t *src = pm_string_source(&n->unescaped);
        size_t len = pm_string_length(&n->unescaped);
        char *buf = malloc(len + 3);
        buf[0] = '"';
        memcpy(buf + 1, src, len);
        buf[len + 1] = '"';
        buf[len + 2] = '\0';
        return buf;
    }

    case PM_GLOBAL_VARIABLE_READ_NODE: {
        pm_global_variable_read_node_t *n = (pm_global_variable_read_node_t *)node;
        char *gname = cstr(ctx, n->name);
        if (strcmp(gname, "$stderr") == 0) { free(gname); return xstrdup("stderr"); }
        if (strcmp(gname, "$stdout") == 0) { free(gname); return xstrdup("stdout"); }
        if (strcmp(gname, "$?") == 0) { free(gname); return xstrdup("sp_last_status"); }
        if (strcmp(gname, "$stdin") == 0) { free(gname); return xstrdup("stdin"); }
        if (strcmp(gname, "$0") == 0 || strcmp(gname, "$PROGRAM_NAME") == 0) {
            free(gname); return xstrdup("sp_program_name");
        }
        free(gname);
        return xstrdup("0 /* unsupported global */");
    }

    case PM_LOCAL_VARIABLE_READ_NODE: {
        pm_local_variable_read_node_t *n = (pm_local_variable_read_node_t *)node;
        char *name = cstr(ctx, n->name);
        char *cn = make_cname(name, false);
        free(name);
        return cn;
    }

    case PM_IT_LOCAL_VARIABLE_READ_NODE:
        /* 'it' keyword (Ruby 3.4+) → same as _1 (first block parameter) */
        return xstrdup("lv__1");

    case PM_CONSTANT_READ_NODE: {
        pm_constant_read_node_t *n = (pm_constant_read_node_t *)node;
        char *name = cstr(ctx, n->name);
        /* ARGV → sp_argv (built at program start from argc/argv) */
        if (strcmp(name, "ARGV") == 0) {
            free(name);
            return xstrdup("sp_argv");
        }
        /* STDERR → stderr, STDIN → stdin, STDOUT → stdout */
        if (strcmp(name, "STDERR") == 0) { free(name); return xstrdup("stderr"); }
        if (strcmp(name, "STDIN") == 0) { free(name); return xstrdup("stdin"); }
        if (strcmp(name, "STDOUT") == 0) { free(name); return xstrdup("stdout"); }
        /* Check if it's a class name used for ::method */
        if (find_class(ctx, name) || find_module(ctx, name)) {
            return name;
        }
        if (ctx->current_module) {
            for (int i = 0; i < ctx->current_module->const_count; i++) {
                if (strcmp(ctx->current_module->consts[i].name, name) == 0) {
                    char *r = sfmt("sp_%s_%s", ctx->current_module->name, name);
                    free(name);
                    return r;
                }
            }
        }
        char *cn = make_cname(name, true);
        free(name);
        return cn;
    }

    case PM_INSTANCE_VARIABLE_READ_NODE: {
        pm_instance_variable_read_node_t *n = (pm_instance_variable_read_node_t *)node;
        char *ivname = cstr(ctx, n->name);
        const char *field = ivname + 1;
        char *r;
        if (ctx->current_module)
            r = sfmt("sp_%s_%s", ctx->current_module->name, field);
        else if (ctx->current_class && ctx->current_class->is_value_type)
            r = sfmt("self.%s", field);
        else
            r = sfmt("self->%s", field);
        free(ivname);
        return r;
    }

    case PM_INSTANCE_VARIABLE_WRITE_NODE: {
        pm_instance_variable_write_node_t *n = (pm_instance_variable_write_node_t *)node;
        char *ivname = cstr(ctx, n->name);
        const char *field = ivname + 1;
        char *val = codegen_expr(ctx, n->value);
        char *r;
        if (ctx->current_module)
            r = sfmt("(sp_%s_%s = %s)", ctx->current_module->name, field, val);
        else if (ctx->current_class && ctx->current_class->is_value_type)
            r = sfmt("(self.%s = %s)", field, val);
        else
            r = sfmt("(self->%s = %s)", field, val);
        free(ivname); free(val);
        return r;
    }

    case PM_INSTANCE_VARIABLE_OPERATOR_WRITE_NODE: {
        pm_instance_variable_operator_write_node_t *n =
            (pm_instance_variable_operator_write_node_t *)node;
        char *ivname = cstr(ctx, n->name);
        const char *field = ivname + 1;  /* skip @ */
        char *op = cstr(ctx, n->binary_operator);
        char *val = codegen_expr(ctx, n->value);
        char *r;
        if (ctx->current_module)
            r = sfmt("(sp_%s_%s %s= %s)", ctx->current_module->name, field, op, val);
        else if (ctx->current_class && ctx->current_class->is_value_type)
            r = sfmt("(self.%s %s= %s)", field, op, val);
        else
            r = sfmt("(self->%s %s= %s)", field, op, val);
        free(ivname); free(op); free(val);
        return r;
    }

    case PM_CALL_NODE: {
        pm_call_node_t *call = (pm_call_node_t *)node;

        /* require/require_relative are compile-time directives — skip */
        if (is_require_relative(ctx, node) || is_require(ctx, node))
            return xstrdup("0");

        char *method = cstr(ctx, call->name);

        /* Skip visibility/mixin directives in expression context */
        if (!call->receiver && (strcmp(method, "private") == 0 ||
            strcmp(method, "protected") == 0 || strcmp(method, "public") == 0 ||
            strcmp(method, "extend") == 0 || strcmp(method, "include") == 0 ||
            strcmp(method, "def_delegators") == 0 ||
            strcmp(method, "frozen_string_literal") == 0)) {
            free(method);
            return xstrdup("0");
        }

        /* N.times.map { |i| expr } → build IntArray from 0..N-1 */
        if (strcmp(method, "map") == 0 && call->receiver &&
            PM_NODE_TYPE(call->receiver) == PM_CALL_NODE && call->block &&
            PM_NODE_TYPE(call->block) == PM_BLOCK_NODE) {
            pm_call_node_t *inner = (pm_call_node_t *)call->receiver;
            if (ceq(ctx, inner->name, "times") && inner->receiver) {
                char *count = codegen_expr(ctx, inner->receiver);
                pm_block_node_t *blk = (pm_block_node_t *)call->block;
                char *bpname = extract_block_param(ctx, blk);
                int tmp = ctx->temp_counter++;
                char *cn = bpname ? make_cname(bpname, false) : sfmt("_ti_%d", tmp);
                emit(ctx, "sp_IntArray *_tmap_%d = sp_IntArray_new();\n", tmp);
                emit(ctx, "for (mrb_int %s = 0; %s < %s; %s++) {\n", cn, cn, count, cn);
                ctx->indent++;
                if (blk->body) {
                    pm_statements_node_t *stmts = (pm_statements_node_t *)blk->body;
                    if (stmts->body.size > 0) {
                        char *val = codegen_expr(ctx, stmts->body.nodes[stmts->body.size - 1]);
                        emit(ctx, "sp_IntArray_push(_tmap_%d, %s);\n", tmp, val);
                        free(val);
                    }
                }
                ctx->indent--;
                emit(ctx, "}\n");
                free(cn); free(bpname); free(count); free(method);
                return sfmt("_tmap_%d", tmp);
            }
        }

        /* Proc#call: receiver.call(arg) → sp_Proc_call(receiver, arg) */
        if (call->receiver && strcmp(method, "call") == 0 && !ctx->lambda_mode) {
            vtype_t recv_t = infer_type(ctx, call->receiver);
            if (recv_t.kind == SPINEL_TYPE_PROC) {
                char *recv = codegen_expr(ctx, call->receiver);
                int argc = call->arguments ? (int)call->arguments->arguments.size : 0;
                char *arg;
                if (argc > 0)
                    arg = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
                else
                    arg = xstrdup("0");
                char *r = sfmt("sp_Proc_call(%s, %s)", recv, arg);
                free(recv); free(arg); free(method);
                return r;
            }
        }

        /* $stdin.getc → getchar() */
        if (call->receiver && PM_NODE_TYPE(call->receiver) == PM_GLOBAL_VARIABLE_READ_NODE &&
            strcmp(method, "getc") == 0) {
            pm_global_variable_read_node_t *gv = (pm_global_variable_read_node_t *)call->receiver;
            char *gname = cstr(ctx, gv->name);
            if (strcmp(gname, "$stdin") == 0) {
                free(gname); free(method);
                return xstrdup("((mrb_int)getchar())");
            }
            free(gname);
        }

        /* $?.success? → (sp_last_status == 0) */
        if (call->receiver && PM_NODE_TYPE(call->receiver) == PM_GLOBAL_VARIABLE_READ_NODE &&
            strcmp(method, "success?") == 0) {
            pm_global_variable_read_node_t *gv = (pm_global_variable_read_node_t *)call->receiver;
            char *gname = cstr(ctx, gv->name);
            if (strcmp(gname, "$?") == 0) {
                free(gname); free(method);
                return xstrdup("(sp_last_status == 0)");
            }
            free(gname);
        }

        /* raise "msg" as expression (used in rescue modifier context) */
        if (!call->receiver && strcmp(method, "raise") == 0) {
            int argc = call->arguments ? (int)call->arguments->arguments.size : 0;
            if (argc >= 2 && PM_NODE_TYPE(call->arguments->arguments.nodes[0]) == PM_CONSTANT_READ_NODE) {
                pm_constant_read_node_t *cr = (pm_constant_read_node_t *)call->arguments->arguments.nodes[0];
                char *cls = cstr(ctx, cr->name);
                char *msg = codegen_expr(ctx, call->arguments->arguments.nodes[1]);
                emit(ctx, "sp_raise_cls(\"%s\", %s);\n", cls, msg);
                free(cls); free(msg);
            } else if (argc >= 1) {
                char *msg = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
                emit(ctx, "sp_raise(%s);\n", msg);
                free(msg);
            } else {
                emit(ctx, "sp_raise(\"RuntimeError\");\n");
            }
            free(method);
            return xstrdup("0");
        }

        /* system("cmd") → (system("cmd") == 0) as expression */
        if (!call->receiver && strcmp(method, "system") == 0 &&
            call->arguments && call->arguments->arguments.size >= 1) {
            char *arg = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
            char *r = sfmt("(sp_last_status = system(%s), sp_last_status == 0)", arg);
            free(arg); free(method);
            return r;
        }

        /* format("fmt", args...) → sp_format("fmt", args...) */
        if (!call->receiver && (strcmp(method, "format") == 0 || strcmp(method, "sprintf") == 0) &&
            call->arguments && call->arguments->arguments.size >= 1) {
            char *fmt = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
            int argc = (int)call->arguments->arguments.size;
            if (argc == 1) {
                char *r = sfmt("sp_format(%s)", fmt);
                free(fmt); free(method);
                return r;
            }
            /* Build args string */
            char *args = xstrdup("");
            for (int i = 1; i < argc; i++) {
                char *a = codegen_expr(ctx, call->arguments->arguments.nodes[i]);
                char *na = sfmt("%s, %s", args, a);
                free(args); free(a);
                args = na;
            }
            char *r = sfmt("sp_format(%s%s)", fmt, args);
            free(fmt); free(args); free(method);
            return r;
        }

        /* proc {} → create sp_Proc from block */
        if (!call->receiver && strcmp(method, "proc") == 0 &&
            call->block && PM_NODE_TYPE(call->block) == PM_BLOCK_NODE && !ctx->lambda_mode) {
            pm_block_node_t *blk = (pm_block_node_t *)call->block;
            int blk_id = ctx->block_counter++;
            ctx->needs_proc = true;

            char *bpname = NULL;
            if (blk->parameters && PM_NODE_TYPE(blk->parameters) == PM_BLOCK_PARAMETERS_NODE) {
                pm_block_parameters_node_t *bp = (pm_block_parameters_node_t *)blk->parameters;
                if (bp->parameters && bp->parameters->requireds.size > 0) {
                    pm_node_t *p = bp->parameters->requireds.nodes[0];
                    if (PM_NODE_TYPE(p) == PM_REQUIRED_PARAMETER_NODE)
                        bpname = cstr(ctx, ((pm_required_parameter_node_t *)p)->name);
                }
            }

            /* Scan for captures */
            capture_list_t local_defs = {.count = 0};
            capture_list_t captures = {.count = 0};
            scan_captures(ctx, (pm_node_t *)blk->body,
                          bpname ? bpname : "", &local_defs, &captures);

            /* Generate block body to temp buffer */
            int saved_indent = ctx->indent;
            int saved_var_count = ctx->var_count;
            ctx->indent = 1;
            if (bpname)
                var_declare(ctx, bpname, vt_prim(SPINEL_TYPE_INTEGER), false);

            char *body_processed = NULL;
            {
                char *body_buf_data = NULL;
                size_t body_buf_size = 0;
                FILE *body_buf = open_memstream(&body_buf_data, &body_buf_size);
                FILE *saved_out = ctx->out;
                ctx->out = body_buf;
                if (blk->body) {
                    /* For proc {}, the last expression is the return value */
                    if (PM_NODE_TYPE((pm_node_t *)blk->body) == PM_STATEMENTS_NODE) {
                        pm_statements_node_t *bstmts = (pm_statements_node_t *)blk->body;
                        for (size_t bi = 0; bi + 1 < bstmts->body.size; bi++)
                            codegen_stmt(ctx, bstmts->body.nodes[bi]);
                        if (bstmts->body.size > 0) {
                            char *rv = codegen_expr(ctx, bstmts->body.nodes[bstmts->body.size - 1]);
                            emit(ctx, "return %s;\n", rv);
                            free(rv);
                        }
                    } else {
                        char *rv = codegen_expr(ctx, (pm_node_t *)blk->body);
                        emit(ctx, "return %s;\n", rv);
                        free(rv);
                    }
                }
                fclose(body_buf);
                ctx->out = saved_out;

                if (body_buf_data) {
                    body_processed = xstrdup(body_buf_data);
                    for (int i = 0; i < captures.count; i++) {
                        char *old_ref = sfmt("lv_%s", captures.names[i]);
                        char *new_ref = sfmt("(*_e->%s)", captures.names[i]);
                        while (1) {
                            char *pos = strstr(body_processed, old_ref);
                            if (!pos) break;
                            size_t prefix_len = pos - body_processed;
                            size_t old_len = strlen(old_ref);
                            size_t new_len = strlen(new_ref);
                            size_t rest_len = strlen(pos + old_len);
                            char *nr = malloc(prefix_len + new_len + rest_len + 1);
                            memcpy(nr, body_processed, prefix_len);
                            memcpy(nr + prefix_len, new_ref, new_len);
                            memcpy(nr + prefix_len + new_len, pos + old_len, rest_len + 1);
                            free(body_processed);
                            body_processed = nr;
                        }
                        free(old_ref); free(new_ref);
                    }
                    free(body_buf_data);
                }
            }
            ctx->indent = saved_indent;
            ctx->var_count = saved_var_count;

            /* Write the block function to block_out */
            if (ctx->block_out) {
                fprintf(ctx->block_out, "typedef struct { ");
                for (int i = 0; i < captures.count; i++)
                    fprintf(ctx->block_out, "mrb_int *%s; ", captures.names[i]);
                if (captures.count == 0) fprintf(ctx->block_out, "int _dummy; ");
                fprintf(ctx->block_out, "} _blk_%d_env;\n", blk_id);

                fprintf(ctx->block_out, "static mrb_int _blk_%d(void *_env, mrb_int _arg) {\n", blk_id);
                fprintf(ctx->block_out, "    _blk_%d_env *_e = (_blk_%d_env *)_env;\n", blk_id, blk_id);
                if (bpname) {
                    char *cn = make_cname(bpname, false);
                    fprintf(ctx->block_out, "    mrb_int %s = _arg;\n", cn);
                    free(cn);
                }
                if (body_processed)
                    fprintf(ctx->block_out, "%s", body_processed);
                fprintf(ctx->block_out, "    return 0;\n");
                fprintf(ctx->block_out, "}\n\n");
            }
            free(body_processed);

            /* Generate sp_Proc allocation at call site */
            int tmp = ctx->temp_counter++;
            if (captures.count > 0) {
                emit(ctx, "_blk_%d_env _env_%d = { ", blk_id, blk_id);
                for (int i = 0; i < captures.count; i++) {
                    char *cn = make_cname(captures.names[i], false);
                    emit_raw(ctx, "%s&%s", i > 0 ? ", " : "", cn);
                    free(cn);
                }
                emit_raw(ctx, " };\n");
                emit(ctx, "sp_Proc *_proc_%d = sp_Proc_new((sp_block_fn)_blk_%d, &_env_%d);\n",
                     tmp, blk_id, blk_id);
            } else {
                emit(ctx, "sp_Proc *_proc_%d = sp_Proc_new((sp_block_fn)_blk_%d, NULL);\n",
                     tmp, blk_id);
            }
            free(bpname); free(method);
            return sfmt("_proc_%d", tmp);
        }

        /* method(:name) → sp_Proc wrapping the named function */
        if (!call->receiver && strcmp(method, "method") == 0 &&
            call->arguments && call->arguments->arguments.size == 1 &&
            PM_NODE_TYPE(call->arguments->arguments.nodes[0]) == PM_SYMBOL_NODE) {
            pm_symbol_node_t *sym = (pm_symbol_node_t *)call->arguments->arguments.nodes[0];
            const uint8_t *src = pm_string_source(&sym->unescaped);
            size_t len = pm_string_length(&sym->unescaped);
            char fname[64];
            snprintf(fname, sizeof(fname), "%.*s", (int)len, src);
            ctx->needs_proc = true;
            free(method);
            /* _meth_adapt_<name> is emitted at file scope by codegen_program */
            return sfmt("sp_Proc_new(_meth_adapt_%s, NULL)", fname);
        }

        /* Proc.new {} → same as proc {} */
        if (call->receiver && PM_NODE_TYPE(call->receiver) == PM_CONSTANT_READ_NODE &&
            strcmp(method, "new") == 0 && !ctx->lambda_mode) {
            pm_constant_read_node_t *cr = (pm_constant_read_node_t *)call->receiver;
            if (ceq(ctx, cr->name, "Proc") &&
                call->block && PM_NODE_TYPE(call->block) == PM_BLOCK_NODE) {
                pm_block_node_t *blk = (pm_block_node_t *)call->block;
                int blk_id = ctx->block_counter++;
                ctx->needs_proc = true;

                char *bpname = NULL;
                if (blk->parameters && PM_NODE_TYPE(blk->parameters) == PM_BLOCK_PARAMETERS_NODE) {
                    pm_block_parameters_node_t *bp = (pm_block_parameters_node_t *)blk->parameters;
                    if (bp->parameters && bp->parameters->requireds.size > 0) {
                        pm_node_t *p = bp->parameters->requireds.nodes[0];
                        if (PM_NODE_TYPE(p) == PM_REQUIRED_PARAMETER_NODE)
                            bpname = cstr(ctx, ((pm_required_parameter_node_t *)p)->name);
                    }
                }

                capture_list_t local_defs = {.count = 0};
                capture_list_t captures = {.count = 0};
                scan_captures(ctx, (pm_node_t *)blk->body,
                              bpname ? bpname : "", &local_defs, &captures);

                int saved_indent = ctx->indent;
                int saved_var_count = ctx->var_count;
                ctx->indent = 1;
                if (bpname)
                    var_declare(ctx, bpname, vt_prim(SPINEL_TYPE_INTEGER), false);

                char *body_processed = NULL;
                {
                    char *body_buf_data = NULL;
                    size_t body_buf_size = 0;
                    FILE *body_buf = open_memstream(&body_buf_data, &body_buf_size);
                    FILE *saved_out = ctx->out;
                    ctx->out = body_buf;
                    if (blk->body) {
                        if (PM_NODE_TYPE((pm_node_t *)blk->body) == PM_STATEMENTS_NODE) {
                            pm_statements_node_t *bstmts = (pm_statements_node_t *)blk->body;
                            for (size_t bi = 0; bi + 1 < bstmts->body.size; bi++)
                                codegen_stmt(ctx, bstmts->body.nodes[bi]);
                            if (bstmts->body.size > 0) {
                                char *rv = codegen_expr(ctx, bstmts->body.nodes[bstmts->body.size - 1]);
                                emit(ctx, "return %s;\n", rv);
                                free(rv);
                            }
                        } else {
                            char *rv = codegen_expr(ctx, (pm_node_t *)blk->body);
                            emit(ctx, "return %s;\n", rv);
                            free(rv);
                        }
                    }
                    fclose(body_buf);
                    ctx->out = saved_out;

                    if (body_buf_data) {
                        body_processed = xstrdup(body_buf_data);
                        for (int i = 0; i < captures.count; i++) {
                            char *old_ref = sfmt("lv_%s", captures.names[i]);
                            char *new_ref = sfmt("(*_e->%s)", captures.names[i]);
                            while (1) {
                                char *pos = strstr(body_processed, old_ref);
                                if (!pos) break;
                                size_t prefix_len = pos - body_processed;
                                size_t old_len = strlen(old_ref);
                                size_t new_len = strlen(new_ref);
                                size_t rest_len = strlen(pos + old_len);
                                char *nr = malloc(prefix_len + new_len + rest_len + 1);
                                memcpy(nr, body_processed, prefix_len);
                                memcpy(nr + prefix_len, new_ref, new_len);
                                memcpy(nr + prefix_len + new_len, pos + old_len, rest_len + 1);
                                free(body_processed);
                                body_processed = nr;
                            }
                            free(old_ref); free(new_ref);
                        }
                        free(body_buf_data);
                    }
                }
                ctx->indent = saved_indent;
                ctx->var_count = saved_var_count;

                if (ctx->block_out) {
                    fprintf(ctx->block_out, "typedef struct { ");
                    for (int i = 0; i < captures.count; i++)
                        fprintf(ctx->block_out, "mrb_int *%s; ", captures.names[i]);
                    if (captures.count == 0) fprintf(ctx->block_out, "int _dummy; ");
                    fprintf(ctx->block_out, "} _blk_%d_env;\n", blk_id);

                    fprintf(ctx->block_out, "static mrb_int _blk_%d(void *_env, mrb_int _arg) {\n", blk_id);
                    fprintf(ctx->block_out, "    _blk_%d_env *_e = (_blk_%d_env *)_env;\n", blk_id, blk_id);
                    if (bpname) {
                        char *cn = make_cname(bpname, false);
                        fprintf(ctx->block_out, "    mrb_int %s = _arg;\n", cn);
                        free(cn);
                    }
                    if (body_processed)
                        fprintf(ctx->block_out, "%s", body_processed);
                    fprintf(ctx->block_out, "    return 0;\n");
                    fprintf(ctx->block_out, "}\n\n");
                }
                free(body_processed);

                int tmp = ctx->temp_counter++;
                if (captures.count > 0) {
                    emit(ctx, "_blk_%d_env _env_%d = { ", blk_id, blk_id);
                    for (int i = 0; i < captures.count; i++) {
                        char *cn = make_cname(captures.names[i], false);
                        emit_raw(ctx, "%s&%s", i > 0 ? ", " : "", cn);
                        free(cn);
                    }
                    emit_raw(ctx, " };\n");
                    emit(ctx, "sp_Proc *_proc_%d = sp_Proc_new((sp_block_fn)_blk_%d, &_env_%d);\n",
                         tmp, blk_id, blk_id);
                } else {
                    emit(ctx, "sp_Proc *_proc_%d = sp_Proc_new((sp_block_fn)_blk_%d, NULL);\n",
                         tmp, blk_id);
                }
                free(bpname); free(method);
                return sfmt("_proc_%d", tmp);
            }
        }

        /* String binary operators: ==, !=, <, >, <=, >=, * */
        if (call->receiver && call->arguments &&
            call->arguments->arguments.size == 1) {
            vtype_t lt = infer_type(ctx, call->receiver);
            vtype_t rt = infer_type(ctx, call->arguments->arguments.nodes[0]);
            if (lt.kind == SPINEL_TYPE_STRING && rt.kind == SPINEL_TYPE_STRING) {
                if (strcmp(method, "==") == 0 || strcmp(method, "!=") == 0 ||
                    strcmp(method, "<") == 0 || strcmp(method, ">") == 0 ||
                    strcmp(method, "<=") == 0 || strcmp(method, ">=") == 0) {
                    char *left = codegen_expr(ctx, call->receiver);
                    char *right = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
                    char *r;
                    if (strcmp(method, "==") == 0)
                        r = sfmt("(strcmp(%s, %s) == 0)", left, right);
                    else if (strcmp(method, "!=") == 0)
                        r = sfmt("(strcmp(%s, %s) != 0)", left, right);
                    else if (strcmp(method, "<") == 0)
                        r = sfmt("(strcmp(%s, %s) < 0)", left, right);
                    else if (strcmp(method, ">") == 0)
                        r = sfmt("(strcmp(%s, %s) > 0)", left, right);
                    else if (strcmp(method, "<=") == 0)
                        r = sfmt("(strcmp(%s, %s) <= 0)", left, right);
                    else
                        r = sfmt("(strcmp(%s, %s) >= 0)", left, right);
                    free(left); free(right); free(method);
                    return r;
                }
            }
            if (lt.kind == SPINEL_TYPE_STRING && rt.kind == SPINEL_TYPE_INTEGER &&
                strcmp(method, "*") == 0) {
                char *left = codegen_expr(ctx, call->receiver);
                char *right = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
                char *r = sfmt("sp_str_repeat(%s, %s)", left, right);
                free(left); free(right); free(method);
                return r;
            }
            /* str =~ /regexp/ → (sp_re_match(_re_N, str) >= 0) */
            if (lt.kind == SPINEL_TYPE_STRING && rt.kind == SPINEL_TYPE_REGEXP &&
                strcmp(method, "=~") == 0) {
                char *left = codegen_expr(ctx, call->receiver);
                char *right = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
                char *r = sfmt("(sp_re_match(%s, %s) >= 0)", right, left);
                free(left); free(right); free(method);
                return r;
            }
            /* pointer == nil → (ptr == NULL) for STRING, OBJECT, ARRAY, etc. */
            if (rt.kind == SPINEL_TYPE_NIL &&
                (lt.kind == SPINEL_TYPE_STRING || lt.kind == SPINEL_TYPE_OBJECT ||
                 lt.kind == SPINEL_TYPE_ARRAY || lt.kind == SPINEL_TYPE_FLOAT_ARRAY ||
                 lt.kind == SPINEL_TYPE_SP_STRING || lt.kind == SPINEL_TYPE_HASH)) {
                if (strcmp(method, "==") == 0 || strcmp(method, "!=") == 0) {
                    char *left = codegen_expr(ctx, call->receiver);
                    char *r = sfmt("(%s %s NULL)", left, strcmp(method, "==") == 0 ? "==" : "!=");
                    free(left); free(method);
                    return r;
                }
            }
            /* nil == pointer (reversed) */
            if (lt.kind == SPINEL_TYPE_NIL &&
                (rt.kind == SPINEL_TYPE_STRING || rt.kind == SPINEL_TYPE_OBJECT ||
                 rt.kind == SPINEL_TYPE_ARRAY || rt.kind == SPINEL_TYPE_FLOAT_ARRAY)) {
                if (strcmp(method, "==") == 0 || strcmp(method, "!=") == 0) {
                    char *right = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
                    char *r = sfmt("(%s %s NULL)", right, strcmp(method, "==") == 0 ? "==" : "!=");
                    free(right); free(method);
                    return r;
                }
            }
        }

        /* Binary operators on numeric types → direct C ops */
        if (call->receiver && call->arguments &&
            call->arguments->arguments.size == 1) {
            vtype_t lt = infer_type(ctx, call->receiver);
            vtype_t rt = infer_type(ctx, call->arguments->arguments.nodes[0]);

            const char *c_op = NULL;
            if (strcmp(method, "+") == 0)  c_op = "+";
            if (strcmp(method, "-") == 0)  c_op = "-";
            if (strcmp(method, "*") == 0)  c_op = "*";
            if (strcmp(method, "/") == 0)  c_op = "/";
            if (strcmp(method, "%") == 0)  c_op = "%";
            if (strcmp(method, "<") == 0)  c_op = "<";
            if (strcmp(method, ">") == 0)  c_op = ">";
            if (strcmp(method, "<=") == 0) c_op = "<=";
            if (strcmp(method, ">=") == 0) c_op = ">=";
            if (strcmp(method, "==") == 0) c_op = "==";
            if (strcmp(method, "!=") == 0) c_op = "!=";
            if (strcmp(method, "<<") == 0) c_op = "<<";
            if (strcmp(method, ">>") == 0) c_op = ">>";
            if (strcmp(method, "|") == 0)  c_op = "|";
            if (strcmp(method, "&") == 0)  c_op = "&";
            if (strcmp(method, "^") == 0)  c_op = "^";

            /* Allow VALUE on one side when the other is numeric (cast to mrb_int) */
            bool lt_ok = vt_is_numeric(lt) || lt.kind == SPINEL_TYPE_BOOLEAN;
            bool rt_ok = vt_is_numeric(rt) || rt.kind == SPINEL_TYPE_BOOLEAN;
            if (!lt_ok && lt.kind == SPINEL_TYPE_VALUE && rt_ok) lt_ok = true;
            if (!rt_ok && rt.kind == SPINEL_TYPE_VALUE && lt_ok) rt_ok = true;
            /* Both VALUE: allow binary ops (VALUE is mrb_int underneath) */
            if (!lt_ok && !rt_ok && lt.kind == SPINEL_TYPE_VALUE && rt.kind == SPINEL_TYPE_VALUE) {
                lt_ok = true; rt_ok = true;
            }

            if (c_op && lt_ok && rt_ok) {
                char *left = codegen_expr(ctx, call->receiver);
                char *right = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
                vtype_t rest = binop_result(lt, rt, method);
                if (rest.kind == SPINEL_TYPE_FLOAT) {
                    if (lt.kind == SPINEL_TYPE_INTEGER) {
                        char *t = sfmt("((mrb_float)%s)", left); free(left); left = t;
                    }
                    if (rt.kind == SPINEL_TYPE_INTEGER) {
                        char *t = sfmt("((mrb_float)%s)", right); free(right); right = t;
                    }
                }
                char *r = sfmt("(%s %s %s)", left, c_op, right);
                free(left); free(right); free(method);
                return r;
            }

            /* POLY binary operators → sp_poly_<op> dispatch */
            if (c_op && (lt.kind == SPINEL_TYPE_POLY || rt.kind == SPINEL_TYPE_POLY)) {
                const char *poly_fn = NULL;
                if (strcmp(method, "+") == 0)  poly_fn = "sp_poly_add";
                else if (strcmp(method, "-") == 0)  poly_fn = "sp_poly_sub";
                else if (strcmp(method, "*") == 0)  poly_fn = "sp_poly_mul";
                else if (strcmp(method, "/") == 0)  poly_fn = "sp_poly_div";
                else if (strcmp(method, ">") == 0)  poly_fn = "sp_poly_gt";
                else if (strcmp(method, "<") == 0)  poly_fn = "sp_poly_lt";
                else if (strcmp(method, ">=") == 0) poly_fn = "sp_poly_ge";
                else if (strcmp(method, "<=") == 0) poly_fn = "sp_poly_le";
                else if (strcmp(method, "==") == 0) poly_fn = "sp_poly_eq";
                else if (strcmp(method, "!=") == 0) poly_fn = "sp_poly_neq";
                if (poly_fn) {
                    char *left = codegen_expr(ctx, call->receiver);
                    char *right = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
                    /* Box non-POLY operands */
                    char *bl = lt.kind == SPINEL_TYPE_POLY ? xstrdup(left)
                             : poly_box_expr_vt(ctx, lt, left);
                    char *br = rt.kind == SPINEL_TYPE_POLY ? xstrdup(right)
                             : poly_box_expr_vt(ctx, rt, right);
                    char *r = sfmt("%s(%s, %s)", poly_fn, bl, br);
                    free(left); free(right); free(bl); free(br); free(method);
                    return r;
                }
            }
        }

        /* ENV['KEY'] → getenv("KEY") */
        if (strcmp(method, "[]") == 0 && call->receiver &&
            PM_NODE_TYPE(call->receiver) == PM_CONSTANT_READ_NODE) {
            pm_constant_read_node_t *cr = (pm_constant_read_node_t *)call->receiver;
            if (ceq(ctx, cr->name, "ENV") && call->arguments &&
                call->arguments->arguments.size == 1) {
                char *key = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
                char *r = sfmt("getenv(%s)", key);
                free(key); free(method);
                return r;
            }
        }

        /* Dir.home → getenv("HOME"), Dir.glob(pat) → sp_Dir_glob(pat) */
        if (call->receiver && PM_NODE_TYPE(call->receiver) == PM_CONSTANT_READ_NODE) {
            pm_constant_read_node_t *cr = (pm_constant_read_node_t *)call->receiver;
            if (ceq(ctx, cr->name, "Dir")) {
                if (strcmp(method, "home") == 0) {
                    free(method);
                    return xstrdup("getenv(\"HOME\")");
                }
                if (strcmp(method, "glob") == 0 && call->arguments &&
                    call->arguments->arguments.size == 1) {
                    char *pat = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
                    char *r = sfmt("sp_Dir_glob(%s)", pat);
                    free(pat); free(method);
                    ctx->needs_str_split = true; /* sp_Dir_glob depends on sp_StrArray */
                    return r;
                }
            }
        }

        /* ARGV.length → sp_argv.len */
        if (call->receiver && PM_NODE_TYPE(call->receiver) == PM_CONSTANT_READ_NODE &&
            strcmp(method, "length") == 0) {
            pm_constant_read_node_t *cr = (pm_constant_read_node_t *)call->receiver;
            if (ceq(ctx, cr->name, "ARGV")) {
                free(method);
                return xstrdup("sp_argv.len");
            }
        }

        /* String indexing: s[n] → sp_str_char_at, s[a..b] → sp_str_slice, s[n,len] → sp_str_slice */
        if (strcmp(method, "[]") == 0 && call->receiver && call->arguments &&
            call->arguments->arguments.size >= 1 && call->arguments->arguments.size <= 2) {
            vtype_t recv_t_pre = infer_type(ctx, call->receiver);
            if (recv_t_pre.kind == SPINEL_TYPE_STRING) {
                pm_node_t *arg = call->arguments->arguments.nodes[0];
                if (PM_NODE_TYPE(arg) == PM_RANGE_NODE) {
                    /* s[start..end] → sp_str_slice */
                    pm_range_node_t *rn = (pm_range_node_t *)arg;
                    char *recv = codegen_expr(ctx, call->receiver);
                    char *start = rn->left ? codegen_expr(ctx, rn->left) : xstrdup("0");
                    if (rn->right) {
                        char *end = codegen_expr(ctx, rn->right);
                        /* Exclusive (..) vs inclusive (...) range */
                        bool exclusive = (rn->base.flags & PM_RANGE_FLAGS_EXCLUDE_END);
                        char *r;
                        if (exclusive) {
                            r = sfmt("sp_str_slice(%s, %s, (%s) - (%s))", recv, start, end, start);
                        } else {
                            r = sfmt("sp_str_slice(%s, %s, (%s) - (%s) + 1)", recv, start, end, start);
                        }
                        free(recv); free(start); free(end); free(method);
                        return r;
                    } else {
                        /* s[start..] → slice to end */
                        char *r = sfmt("sp_str_slice(%s, %s, (mrb_int)strlen(%s))", recv, start, recv);
                        free(recv); free(start); free(method);
                        return r;
                    }
                }
                /* s[n, len] — two-arg form handled via slice */
                if (call->arguments->arguments.size == 2) {
                    char *recv = codegen_expr(ctx, call->receiver);
                    char *start = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
                    char *len = codegen_expr(ctx, call->arguments->arguments.nodes[1]);
                    char *r = sfmt("sp_str_slice(%s, %s, %s)", recv, start, len);
                    free(recv); free(start); free(len); free(method);
                    return r;
                }
                char *recv = codegen_expr(ctx, call->receiver);
                char *idx = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
                char *r = sfmt("sp_str_char_at(%s, %s)", recv, idx);
                free(recv); free(idx); free(method);
                return r;
            }
            if (recv_t_pre.kind == SPINEL_TYPE_SP_STRING) {
                char *recv = codegen_expr(ctx, call->receiver);
                char *idx = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
                char *r = sfmt("sp_String_char_at(%s, %s)", recv, idx);
                free(recv); free(idx); free(method);
                return r;
            }
        }

        /* Array indexing / Proc call: obj[arg] */
        if (strcmp(method, "[]") == 0 && call->receiver && call->arguments &&
            call->arguments->arguments.size == 1) {
            vtype_t recv_t = infer_type(ctx, call->receiver);
            /* In lambda mode: inside lambda bodies, ALL [] are proc calls (sp_call)
             * because every variable in a lambda body is sp_Val*.
             * At top level in lambda mode, use sp_call for proc/value/unknown types. */
            if (ctx->lambda_mode &&
                (ctx->lambda_scope_depth > 0 ||
                 recv_t.kind == SPINEL_TYPE_PROC || recv_t.kind == SPINEL_TYPE_VALUE ||
                 recv_t.kind == SPINEL_TYPE_UNKNOWN)) {
                char *recv = codegen_expr(ctx, call->receiver);
                char *arg = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
                char *r = sfmt("sp_call(%s, %s)", recv, arg);
                free(recv); free(arg); free(method);
                return r;
            }
            if (recv_t.kind == SPINEL_TYPE_FLOAT_ARRAY) {
                char *recv = codegen_expr(ctx, call->receiver);
                char *idx = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
                char *r = sfmt("sp_FloatArray_get(%s, %s)", recv, idx);
                free(recv); free(idx); free(method);
                return r;
            }
            if (recv_t.kind != SPINEL_TYPE_ARRAY && recv_t.kind != SPINEL_TYPE_HASH &&
                recv_t.kind != SPINEL_TYPE_RB_HASH && recv_t.kind != SPINEL_TYPE_STR_ARRAY &&
                recv_t.kind != SPINEL_TYPE_RB_ARRAY && recv_t.kind != SPINEL_TYPE_FLOAT_ARRAY) {
                char *recv = codegen_expr(ctx, call->receiver);
                char *idx = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
                char *r = sfmt("%s[%s]", recv, idx);
                free(recv); free(idx); free(method);
                return r;
            }
        }

        /* Array index assignment: obj[index] = val → obj[index] = val */
        if (strcmp(method, "[]=") == 0 && call->receiver && call->arguments &&
            call->arguments->arguments.size == 2) {
            vtype_t recv_t = infer_type(ctx, call->receiver);
            if (recv_t.kind == SPINEL_TYPE_ARRAY) {
                /* IntArray: cells[idx] = val → sp_IntArray_set */
                char *recv = codegen_expr(ctx, call->receiver);
                char *idx = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
                char *val = codegen_expr(ctx, call->arguments->arguments.nodes[1]);
                char *r = sfmt("sp_IntArray_set(%s, %s, %s)", recv, idx, val);
                free(recv); free(idx); free(val); free(method);
                return r;
            }
            if (recv_t.kind == SPINEL_TYPE_FLOAT_ARRAY) {
                /* FloatArray: arr[idx] = val → sp_FloatArray_set */
                char *recv = codegen_expr(ctx, call->receiver);
                char *idx = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
                char *val = codegen_expr(ctx, call->arguments->arguments.nodes[1]);
                char *r = sfmt("sp_FloatArray_set(%s, %s, %s)", recv, idx, val);
                free(recv); free(idx); free(val); free(method);
                return r;
            }
            if (recv_t.kind != SPINEL_TYPE_HASH && recv_t.kind != SPINEL_TYPE_RB_HASH) {
                char *recv = codegen_expr(ctx, call->receiver);
                char *idx = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
                char *val = codegen_expr(ctx, call->arguments->arguments.nodes[1]);
                char *r = sfmt("(%s[%s] = %s)", recv, idx, val);
                free(recv); free(idx); free(val); free(method);
                return r;
            }
        }

        /* Unary minus: -expr */
        if (strcmp(method, "-@") == 0 && call->receiver && !call->arguments) {
            char *recv = codegen_expr(ctx, call->receiver);
            char *r = sfmt("(-%s)", recv);
            free(recv); free(method);
            return r;
        }

        /* Unary not: !expr (also used for `not expr`) */
        if (strcmp(method, "!") == 0 && call->receiver && !call->arguments) {
            char *recv = codegen_expr(ctx, call->receiver);
            char *r = sfmt("(!%s)", recv);
            free(recv); free(method);
            return r;
        }

        /* Range#to_a → sp_IntArray_from_range(start, end) */
        if (strcmp(method, "to_a") == 0 && call->receiver) {
            pm_node_t *recv_node = call->receiver;
            /* Unwrap parentheses */
            while (PM_NODE_TYPE(recv_node) == PM_PARENTHESES_NODE) {
                pm_parentheses_node_t *pn = (pm_parentheses_node_t *)recv_node;
                if (pn->body) recv_node = pn->body;
                else break;
            }
            /* Unwrap statements */
            if (PM_NODE_TYPE(recv_node) == PM_STATEMENTS_NODE) {
                pm_statements_node_t *ss = (pm_statements_node_t *)recv_node;
                if (ss->body.size == 1) recv_node = ss->body.nodes[0];
            }
            if (PM_NODE_TYPE(recv_node) == PM_RANGE_NODE) {
                pm_range_node_t *rng = (pm_range_node_t *)recv_node;
                char *left = codegen_expr(ctx, rng->left);
                char *right = codegen_expr(ctx, rng->right);
                char *r = sfmt("sp_IntArray_from_range(%s, %s)", left, right);
                free(left); free(right); free(method);
                return r;
            }
        }

        /* Constructor: ClassName.new(args) */
        if (strcmp(method, "new") == 0 && call->receiver &&
            PM_NODE_TYPE(call->receiver) == PM_CONSTANT_READ_NODE) {
            pm_constant_read_node_t *cr = (pm_constant_read_node_t *)call->receiver;
            char *cls_name = cstr(ctx, cr->name);
            class_info_t *cls = find_class(ctx, cls_name);
            if (cls) {
                int argc = call->arguments ? (int)call->arguments->arguments.size : 0;
                char *args = xstrdup("");

                /* Check if this is a keyword_init Struct */
                method_info_t *init_m = NULL;
                for (int mi = 0; mi < cls->method_count; mi++) {
                    if (strcmp(cls->methods[mi].name, "initialize") == 0) {
                        init_m = &cls->methods[mi];
                        break;
                    }
                }
                bool kw_init = init_m && init_m->param_count > 0 && init_m->params[0].is_keyword;

                if (kw_init && argc == 1 &&
                    PM_NODE_TYPE(call->arguments->arguments.nodes[0]) == PM_KEYWORD_HASH_NODE) {
                    /* keyword_init Struct: Session.new(pid: 123, name: "test")
                     * Unpack KeywordHashNode and reorder args by field position */
                    pm_keyword_hash_node_t *kwh = (pm_keyword_hash_node_t *)call->arguments->arguments.nodes[0];
                    int nkw = (int)kwh->elements.size;

                    /* For each param in field order, find matching keyword arg */
                    for (int pi = 0; pi < init_m->param_count; pi++) {
                        char *val = NULL;
                        for (int ki = 0; ki < nkw; ki++) {
                            if (PM_NODE_TYPE(kwh->elements.nodes[ki]) != PM_ASSOC_NODE) continue;
                            pm_assoc_node_t *assoc = (pm_assoc_node_t *)kwh->elements.nodes[ki];
                            /* Key is a SymbolNode */
                            if (PM_NODE_TYPE(assoc->key) == PM_SYMBOL_NODE) {
                                pm_symbol_node_t *ksym = (pm_symbol_node_t *)assoc->key;
                                const uint8_t *ksrc = pm_string_source(&ksym->unescaped);
                                size_t klen = pm_string_length(&ksym->unescaped);
                                char kname[64];
                                snprintf(kname, sizeof(kname), "%.*s", (int)klen, ksrc);
                                if (strcmp(kname, init_m->params[pi].name) == 0) {
                                    val = codegen_expr(ctx, assoc->value);
                                    break;
                                }
                            }
                        }
                        if (!val) val = xstrdup("0"); /* default if missing */
                        char *na = sfmt("%s%s%s", args, pi > 0 ? ", " : "", val);
                        free(args); free(val);
                        args = na;
                    }
                } else {
                    for (int i = 0; i < argc; i++) {
                        char *a = codegen_expr(ctx, call->arguments->arguments.nodes[i]);
                        char *na = sfmt("%s%s%s", args, i > 0 ? ", " : "", a);
                        free(args); free(a);
                        args = na;
                    }
                    /* Fill in default values for optional params */
                    if (init_m) {
                        pm_parser_t *saved_p = ctx->parser;
                        if (init_m->origin_parser) ctx->parser = init_m->origin_parser;
                        for (int i = argc; i < init_m->param_count; i++) {
                            if (init_m->params[i].is_optional && init_m->params[i].default_node) {
                                char *def = codegen_expr(ctx, (pm_node_t *)init_m->params[i].default_node);
                                char *na = sfmt("%s%s%s", args, i > 0 ? ", " : "", def);
                                free(args); free(def);
                                args = na;
                            }
                        }
                        ctx->parser = saved_p;
                    }
                }
                char *r = sfmt("sp_%s_new(%s)", cls_name, args);
                free(cls_name); free(args); free(method);
                return r;
            }
            /* Array.new */
            if (strcmp(cls_name, "Array") == 0) {
                int argc = call->arguments ? (int)call->arguments->arguments.size : 0;
                if (argc == 0 && !ctx->current_class) {
                    /* Dynamic sp_IntArray (top-level / regular functions) */
                    free(cls_name); free(method);
                    return xstrdup("sp_IntArray_new()");
                }
                if (argc == 2) {
                    /* Array.new(size, default_val) */
                    vtype_t dv_t = infer_type(ctx, call->arguments->arguments.nodes[1]);
                    char *sz = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
                    char *dv = codegen_expr(ctx, call->arguments->arguments.nodes[1]);
                    int tmp = ctx->temp_counter++;
                    if (dv_t.kind == SPINEL_TYPE_FLOAT) {
                        emit(ctx, "sp_FloatArray *_anew_%d = sp_FloatArray_new();\n", tmp);
                        emit(ctx, "for (mrb_int _ai_%d = 0; _ai_%d < %s; _ai_%d++) sp_FloatArray_push(_anew_%d, %s);\n",
                             tmp, tmp, sz, tmp, tmp, dv);
                    } else {
                        emit(ctx, "sp_IntArray *_anew_%d = sp_IntArray_new();\n", tmp);
                        emit(ctx, "for (mrb_int _ai_%d = 0; _ai_%d < %s; _ai_%d++) sp_IntArray_push(_anew_%d, %s);\n",
                             tmp, tmp, sz, tmp, tmp, dv);
                    }
                    free(sz); free(dv); free(cls_name); free(method);
                    return sfmt("_anew_%d", tmp);
                }
                if (argc == 1) {
                    /* Array.new(size) — zero-filled */
                    char *sz = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
                    int tmp = ctx->temp_counter++;
                    emit(ctx, "sp_IntArray *_anew_%d = sp_IntArray_new();\n", tmp);
                    emit(ctx, "for (mrb_int _ai_%d = 0; _ai_%d < %s; _ai_%d++) sp_IntArray_push(_anew_%d, 0);\n",
                         tmp, tmp, sz, tmp, tmp);
                    free(sz); free(cls_name); free(method);
                    return sfmt("_anew_%d", tmp);
                }
                /* Fixed-size C array (inside class) or with size arg — skip */
                free(cls_name); free(method);
                return xstrdup("/* array_init */");
            }
            /* Hash.new / Hash.new(default) */
            if (strcmp(cls_name, "Hash") == 0) {
                int argc = call->arguments ? (int)call->arguments->arguments.size : 0;
                ctx->needs_hash = true;
                ctx->needs_gc = true;
                if (argc == 1) {
                    char *dv = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
                    char *r = sfmt("sp_StrIntHash_new_with_default(%s)", dv);
                    free(dv); free(cls_name); free(method);
                    return r;
                }
                free(cls_name); free(method);
                return xstrdup("sp_StrIntHash_new()");
            }
            free(cls_name);
        }
        /* Constructor: Module::ClassName.new(args) */
        if (strcmp(method, "new") == 0 && call->receiver &&
            PM_NODE_TYPE(call->receiver) == PM_CONSTANT_PATH_NODE) {
            pm_constant_path_node_t *cp = (pm_constant_path_node_t *)call->receiver;
            char *cls_name = cstr(ctx, cp->name);
            class_info_t *cls = find_class(ctx, cls_name);
            if (cls) {
                int argc = call->arguments ? (int)call->arguments->arguments.size : 0;
                char *args = xstrdup("");
                /* Check for keyword_init */
                method_info_t *init_m = NULL;
                for (int mi = 0; mi < cls->method_count; mi++)
                    if (strcmp(cls->methods[mi].name, "initialize") == 0) { init_m = &cls->methods[mi]; break; }
                bool kw_init = init_m && init_m->param_count > 0 && init_m->params[0].is_keyword;
                if (kw_init && argc == 1 &&
                    PM_NODE_TYPE(call->arguments->arguments.nodes[0]) == PM_KEYWORD_HASH_NODE) {
                    pm_keyword_hash_node_t *kwh = (pm_keyword_hash_node_t *)call->arguments->arguments.nodes[0];
                    for (int pi = 0; pi < init_m->param_count; pi++) {
                        char *val = NULL;
                        for (int ki = 0; ki < (int)kwh->elements.size; ki++) {
                            if (PM_NODE_TYPE(kwh->elements.nodes[ki]) != PM_ASSOC_NODE) continue;
                            pm_assoc_node_t *assoc = (pm_assoc_node_t *)kwh->elements.nodes[ki];
                            if (PM_NODE_TYPE(assoc->key) == PM_SYMBOL_NODE) {
                                pm_symbol_node_t *ksym = (pm_symbol_node_t *)assoc->key;
                                const uint8_t *ksrc = pm_string_source(&ksym->unescaped);
                                size_t klen = pm_string_length(&ksym->unescaped);
                                char kname[64]; snprintf(kname, sizeof(kname), "%.*s", (int)klen, ksrc);
                                if (strcmp(kname, init_m->params[pi].name) == 0) { val = codegen_expr(ctx, assoc->value); break; }
                            }
                        }
                        if (!val) val = xstrdup("0");
                        char *na = sfmt("%s%s%s", args, pi > 0 ? ", " : "", val);
                        free(args); free(val); args = na;
                    }
                } else {
                    for (int i = 0; i < argc; i++) {
                        char *a = codegen_expr(ctx, call->arguments->arguments.nodes[i]);
                        char *na = sfmt("%s%s%s", args, i > 0 ? ", " : "", a);
                        free(args); free(a); args = na;
                    }
                    /* Fill in default values for optional params */
                    if (init_m) {
                        pm_parser_t *saved_p = ctx->parser;
                        if (init_m->origin_parser) ctx->parser = init_m->origin_parser;
                        for (int i = argc; i < init_m->param_count; i++) {
                            if (init_m->params[i].is_optional && init_m->params[i].default_node) {
                                char *def = codegen_expr(ctx, (pm_node_t *)init_m->params[i].default_node);
                                char *na = sfmt("%s%s%s", args, i > 0 ? ", " : "", def);
                                free(args); free(def);
                                args = na;
                            }
                        }
                        ctx->parser = saved_p;
                    }
                }
                char *r = sfmt("sp_%s_new(%s)", cls_name, args);
                free(cls_name); free(args); free(method);
                return r;
            }
            free(cls_name);
        }

        /* Method call on typed receiver */
        if (call->receiver) {
            vtype_t recv_t = infer_type(ctx, call->receiver);

            /* Math.sqrt/cos/sin */
            if (PM_NODE_TYPE(call->receiver) == PM_CONSTANT_READ_NODE) {
                pm_constant_read_node_t *cr = (pm_constant_read_node_t *)call->receiver;
                if (ceq(ctx, cr->name, "Math")) {
                    if (call->arguments && call->arguments->arguments.size == 1) {
                        char *arg = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
                        char *r = sfmt("%s(%s)", method, arg); /* sqrt, cos, sin */
                        free(arg); free(method);
                        return r;
                    }
                }
                /* File class methods */
                if (ceq(ctx, cr->name, "File")) {
                    if (strcmp(method, "read") == 0 && call->arguments &&
                        call->arguments->arguments.size == 1) {
                        char *path = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
                        char *r = sfmt("sp_File_read(%s)", path);
                        free(path); free(method);
                        return r;
                    }
                    if (strcmp(method, "write") == 0 && call->arguments &&
                        call->arguments->arguments.size == 2) {
                        char *path = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
                        char *data = codegen_expr(ctx, call->arguments->arguments.nodes[1]);
                        char *r = sfmt("sp_File_write(%s, %s)", path, data);
                        free(path); free(data); free(method);
                        return r;
                    }
                    if ((strcmp(method, "exist?") == 0 || strcmp(method, "exists?") == 0) &&
                        call->arguments && call->arguments->arguments.size == 1) {
                        char *path = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
                        char *r = sfmt("sp_File_exist(%s)", path);
                        free(path); free(method);
                        return r;
                    }
                    if (strcmp(method, "delete") == 0 && call->arguments &&
                        call->arguments->arguments.size == 1) {
                        char *path = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
                        char *r = sfmt("sp_File_delete(%s)", path);
                        free(path); free(method);
                        return r;
                    }
                    if (strcmp(method, "join") == 0 && call->arguments &&
                        call->arguments->arguments.size == 2) {
                        char *a = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
                        char *b = codegen_expr(ctx, call->arguments->arguments.nodes[1]);
                        char *r = sfmt("sp_File_join(%s, %s)", a, b);
                        free(a); free(b); free(method);
                        return r;
                    }
                    if (strcmp(method, "expand_path") == 0 && call->arguments &&
                        call->arguments->arguments.size == 1) {
                        char *path = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
                        char *r = sfmt("sp_File_expand_path(%s)", path);
                        free(path); free(method);
                        return r;
                    }
                    if (strcmp(method, "basename") == 0 && call->arguments &&
                        call->arguments->arguments.size == 1) {
                        char *path = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
                        char *r = sfmt("sp_File_basename(%s)", path);
                        free(path); free(method);
                        return r;
                    }
                    if (strcmp(method, "dirname") == 0 && call->arguments &&
                        call->arguments->arguments.size == 1) {
                        char *path = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
                        char *r = sfmt("sp_File_dirname(%s)", path);
                        free(path); free(method);
                        return r;
                    }
                    if (strcmp(method, "rename") == 0 && call->arguments &&
                        call->arguments->arguments.size == 2) {
                        char *a = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
                        char *b = codegen_expr(ctx, call->arguments->arguments.nodes[1]);
                        char *r = sfmt("sp_File_rename(%s, %s)", a, b);
                        free(a); free(b); free(method);
                        return r;
                    }
                    if (strcmp(method, "size") == 0 && call->arguments &&
                        call->arguments->arguments.size == 1) {
                        char *path = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
                        char *r = sfmt("sp_File_size(%s)", path);
                        free(path); free(method);
                        return r;
                    }
                    if (strcmp(method, "mtime") == 0 && call->arguments &&
                        call->arguments->arguments.size == 1) {
                        char *path = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
                        char *r = sfmt("sp_File_mtime(%s)", path);
                        free(path); free(method);
                        return r;
                    }
                    if ((strcmp(method, "stat") == 0 || strcmp(method, "ctime") == 0) &&
                        call->arguments && call->arguments->arguments.size == 1) {
                        char *path = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
                        char *r = sfmt("sp_File_ctime(%s)", path);
                        free(path); free(method);
                        return r;
                    }
                    if (strcmp(method, "readlink") == 0 && call->arguments &&
                        call->arguments->arguments.size == 1) {
                        char *path = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
                        char *r = sfmt("sp_File_readlink(%s)", path);
                        free(path); free(method);
                        return r;
                    }
                }
                /* Time class methods */
                if (ceq(ctx, cr->name, "Time")) {
                    if (strcmp(method, "now") == 0) {
                        free(method);
                        return xstrdup("sp_Time_now()");
                    }
                    if (strcmp(method, "at") == 0 && call->arguments &&
                        call->arguments->arguments.size == 1) {
                        char *arg = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
                        char *r = sfmt("sp_Time_at(%s)", arg);
                        free(arg); free(method);
                        return r;
                    }
                }
                /* Rand::rand */
                if (ceq(ctx, cr->name, "Rand")) {
                    char *r = sfmt("sp_Rand_%s()", method);
                    free(method);
                    return r;
                }
                /* Class method call: ClassName.method(args) */
                {
                    char *cls_name = cstr(ctx, cr->name);
                    class_info_t *cls = find_class(ctx, cls_name);
                    if (cls) {
                        method_info_t *cm = find_method(cls, method);
                        if (cm && cm->is_class_method) {
                            int argc = call->arguments ? (int)call->arguments->arguments.size : 0;
                            char *args = xstrdup("");
                            for (int i = 0; i < argc; i++) {
                                char *a = codegen_expr(ctx, call->arguments->arguments.nodes[i]);
                                char *na = sfmt("%s%s%s", args, i > 0 ? ", " : "", a);
                                free(args); free(a);
                                args = na;
                            }
                            char *r = sfmt("sp_%s_%s(%s)", cls_name, sanitize_method(method), args);
                            free(cls_name); free(args); free(method);
                            return r;
                        }
                    }
                    /* Module method call: ModuleName.method(args) */
                    module_info_t *mod = find_module(ctx, cls_name);
                    if (mod) {
                        for (int mi2 = 0; mi2 < mod->method_count; mi2++) {
                            if (strcmp(mod->methods[mi2].name, method) == 0 &&
                                mod->methods[mi2].is_class_method) {
                                int argc = call->arguments ? (int)call->arguments->arguments.size : 0;
                                char *args = xstrdup("");
                                for (int i = 0; i < argc; i++) {
                                    char *a = codegen_expr(ctx, call->arguments->arguments.nodes[i]);
                                    char *na = sfmt("%s%s%s", args, i > 0 ? ", " : "", a);
                                    free(args); free(a);
                                    args = na;
                                }
                                char *r = sfmt("sp_%s_%s(%s)", cls_name, sanitize_method(method), args);
                                free(cls_name); free(args); free(method);
                                return r;
                            }
                        }
                    }
                    free(cls_name);
                }
            }

            /* Hash#keys.length → sp_StrIntHash_length (chained call optimization) */
            if (recv_t.kind == SPINEL_TYPE_ARRAY && strcmp(method, "length") == 0 &&
                PM_NODE_TYPE(call->receiver) == PM_CALL_NODE) {
                pm_call_node_t *inner = (pm_call_node_t *)call->receiver;
                if (ceq(ctx, inner->name, "keys") && inner->receiver) {
                    vtype_t inner_recv_t = infer_type(ctx, inner->receiver);
                    if (inner_recv_t.kind == SPINEL_TYPE_HASH) {
                        char *hrecv = codegen_expr(ctx, inner->receiver);
                        char *r = sfmt("sp_StrIntHash_length(%s)", hrecv);
                        free(hrecv); free(method);
                        return r;
                    }
                }
            }

            /* sp_IntArray method calls */
            if (recv_t.kind == SPINEL_TYPE_ARRAY) {
                char *recv = codegen_expr(ctx, call->receiver);
                char *r = NULL;
                if (strcmp(method, "dup") == 0)
                    r = sfmt("sp_IntArray_dup(%s)", recv);
                else if (strcmp(method, "empty?") == 0)
                    r = sfmt("sp_IntArray_empty(%s)", recv);
                else if (strcmp(method, "shift") == 0)
                    r = sfmt("sp_IntArray_shift(%s)", recv);
                else if (strcmp(method, "pop") == 0)
                    r = sfmt("sp_IntArray_pop(%s)", recv);
                else if (strcmp(method, "length") == 0)
                    r = sfmt("sp_IntArray_length(%s)", recv);
                else if (strcmp(method, "reverse!") == 0)
                    r = sfmt("sp_IntArray_reverse_bang(%s)", recv);
                else if (strcmp(method, "reverse") == 0) {
                    int tmp = ctx->temp_counter++;
                    emit(ctx, "sp_IntArray *_rev_%d = sp_IntArray_new();\n", tmp);
                    emit(ctx, "for (mrb_int _ri_%d = sp_IntArray_length(%s) - 1; _ri_%d >= 0; _ri_%d--)\n", tmp, recv, tmp, tmp);
                    emit(ctx, "  sp_IntArray_push(_rev_%d, sp_IntArray_get(%s, _ri_%d));\n", tmp, recv, tmp);
                    r = sfmt("_rev_%d", tmp);
                }
                else if (strcmp(method, "compact") == 0)
                    r = sfmt("sp_IntArray_dup(%s)", recv);
                else if (strcmp(method, "flatten") == 0)
                    r = sfmt("sp_IntArray_dup(%s)", recv);
                else if (strcmp(method, "unshift") == 0 && call->arguments &&
                         call->arguments->arguments.size == 1) {
                    char *arg = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
                    r = sfmt("sp_IntArray_unshift(%s, %s)", recv, arg);
                    free(arg);
                }
                else if (strcmp(method, "sort") == 0)
                    r = sfmt("sp_IntArray_sort(%s)", recv);
                else if (strcmp(method, "sort!") == 0)
                    r = sfmt("sp_IntArray_sort_bang(%s)", recv);
                else if (strcmp(method, "uniq!") == 0) {
                    /* In-place unique: O(n^2) remove duplicates */
                    int tmp = ctx->temp_counter++;
                    emit(ctx, "{ mrb_int _uw_%d = 0;\n", tmp);
                    emit(ctx, "  for (mrb_int _ui_%d = 0; _ui_%d < sp_IntArray_length(%s); _ui_%d++) {\n", tmp, tmp, recv, tmp);
                    emit(ctx, "    mrb_int _uv_%d = sp_IntArray_get(%s, _ui_%d); mrb_bool _dup_%d = FALSE;\n", tmp, recv, tmp, tmp);
                    emit(ctx, "    for (mrb_int _uj_%d = 0; _uj_%d < _uw_%d; _uj_%d++)\n", tmp, tmp, tmp, tmp);
                    emit(ctx, "      if (%s->data[%s->start + _uj_%d] == _uv_%d) { _dup_%d = TRUE; break; }\n", recv, recv, tmp, tmp, tmp);
                    emit(ctx, "    if (!_dup_%d) %s->data[%s->start + _uw_%d++] = _uv_%d;\n", tmp, recv, recv, tmp, tmp);
                    emit(ctx, "  }\n");
                    emit(ctx, "  %s->len = _uw_%d;\n", recv, tmp);
                    emit(ctx, "}\n");
                    r = sfmt("%s", recv);
                }
                else if (strcmp(method, "flatten!") == 0)
                    r = sfmt("%s", recv);  /* no-op for IntArray (already flat) */
                else if (strcmp(method, "min") == 0) {
                    int tmp = ctx->temp_counter++;
                    emit(ctx, "mrb_int _min_%d = sp_IntArray_get(%s, 0);\n", tmp, recv);
                    emit(ctx, "for (mrb_int _mi_%d = 1; _mi_%d < sp_IntArray_length(%s); _mi_%d++) {\n", tmp, tmp, recv, tmp);
                    emit(ctx, "  mrb_int _v_%d = sp_IntArray_get(%s, _mi_%d);\n", tmp, recv, tmp);
                    emit(ctx, "  if (_v_%d < _min_%d) _min_%d = _v_%d;\n", tmp, tmp, tmp, tmp);
                    emit(ctx, "}\n");
                    r = sfmt("_min_%d", tmp);
                }
                else if (strcmp(method, "max") == 0) {
                    int tmp = ctx->temp_counter++;
                    emit(ctx, "mrb_int _max_%d = sp_IntArray_get(%s, 0);\n", tmp, recv);
                    emit(ctx, "for (mrb_int _mi_%d = 1; _mi_%d < sp_IntArray_length(%s); _mi_%d++) {\n", tmp, tmp, recv, tmp);
                    emit(ctx, "  mrb_int _v_%d = sp_IntArray_get(%s, _mi_%d);\n", tmp, recv, tmp);
                    emit(ctx, "  if (_v_%d > _max_%d) _max_%d = _v_%d;\n", tmp, tmp, tmp, tmp);
                    emit(ctx, "}\n");
                    r = sfmt("_max_%d", tmp);
                }
                else if (strcmp(method, "sum") == 0) {
                    int tmp = ctx->temp_counter++;
                    emit(ctx, "mrb_int _sum_%d = 0;\n", tmp);
                    emit(ctx, "for (mrb_int _si_%d = 0; _si_%d < sp_IntArray_length(%s); _si_%d++)\n", tmp, tmp, recv, tmp);
                    emit(ctx, "  _sum_%d += sp_IntArray_get(%s, _si_%d);\n", tmp, recv, tmp);
                    r = sfmt("_sum_%d", tmp);
                }
                else if (strcmp(method, "first") == 0)
                    r = sfmt("sp_IntArray_get(%s, 0)", recv);
                else if (strcmp(method, "last") == 0)
                    r = sfmt("sp_IntArray_get(%s, sp_IntArray_length(%s) - 1)", recv, recv);
                else if (strcmp(method, "include?") == 0 && call->arguments &&
                         call->arguments->arguments.size == 1) {
                    char *arg = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
                    int tmp = ctx->temp_counter++;
                    emit(ctx, "mrb_bool _incl_%d = FALSE; { mrb_int _ii_%d;\n", tmp, tmp);
                    emit(ctx, "  for (_ii_%d = 0; _ii_%d < sp_IntArray_length(%s); _ii_%d++)\n", tmp, tmp, recv, tmp);
                    emit(ctx, "    if (sp_IntArray_get(%s, _ii_%d) == %s) { _incl_%d = TRUE; break; }\n", recv, tmp, arg, tmp);
                    emit(ctx, "}\n");
                    free(arg);
                    r = sfmt("_incl_%d", tmp);
                }
                else if (strcmp(method, "uniq") == 0)
                    r = sfmt("sp_IntArray_uniq(%s)", recv);
                else if (strcmp(method, "join") == 0 && call->arguments &&
                         call->arguments->arguments.size == 1) {
                    char *arg = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
                    r = sfmt("sp_IntArray_join(%s, %s)", recv, arg);
                    free(arg);
                }
                else if ((strcmp(method, "push") == 0 || strcmp(method, "<<") == 0) &&
                         call->arguments &&
                         call->arguments->arguments.size == 1) {
                    char *arg = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
                    r = sfmt("(sp_IntArray_push(%s, %s), %s)", recv, arg, recv);
                    free(arg);
                }
                else if (strcmp(method, "insert") == 0 && call->arguments &&
                         call->arguments->arguments.size == 2) {
                    char *idx = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
                    char *val = codegen_expr(ctx, call->arguments->arguments.nodes[1]);
                    r = sfmt("(sp_IntArray_insert(%s, %s, %s), %s)", recv, idx, val, recv);
                    free(idx); free(val);
                }
                else if (strcmp(method, "[]") == 0 && call->arguments &&
                         call->arguments->arguments.size == 1) {
                    char *idx = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
                    r = sfmt("sp_IntArray_get(%s, %s)", recv, idx);
                    free(idx);
                }
                else if (strcmp(method, "!=") == 0 && call->arguments &&
                         call->arguments->arguments.size == 1) {
                    char *arg = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
                    r = sfmt("sp_IntArray_neq(%s, %s)", recv, arg);
                    free(arg);
                }
                else if (strcmp(method, "==") == 0 && call->arguments &&
                         call->arguments->arguments.size == 1) {
                    char *arg = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
                    r = sfmt("(!sp_IntArray_neq(%s, %s))", recv, arg);
                    free(arg);
                }
                else if (strcmp(method, "any?") == 0 && call->block &&
                         PM_NODE_TYPE(call->block) == PM_BLOCK_NODE) {
                    pm_block_node_t *blk = (pm_block_node_t *)call->block;
                    char *bpname = extract_block_param(ctx, blk);
                    int tmp = ctx->temp_counter++;
                    emit(ctx, "mrb_bool _any_%d = FALSE;\n", tmp);
                    emit(ctx, "for (mrb_int _ai_%d = 0; _ai_%d < sp_IntArray_length(%s); _ai_%d++) {\n", tmp, tmp, recv, tmp);
                    ctx->indent++;
                    if (bpname) {
                        char *cn = make_cname(bpname, false);
                        emit(ctx, "mrb_int %s = sp_IntArray_get(%s, _ai_%d);\n", cn, recv, tmp);
                        free(cn);
                    }
                    if (blk->body) {
                        pm_statements_node_t *stmts = (pm_statements_node_t *)blk->body;
                        if (stmts->body.size > 0) {
                            char *cond = codegen_expr(ctx, stmts->body.nodes[stmts->body.size - 1]);
                            emit(ctx, "if (%s) { _any_%d = TRUE; break; }\n", cond, tmp);
                            free(cond);
                        }
                    }
                    ctx->indent--;
                    emit(ctx, "}\n");
                    free(bpname);
                    r = sfmt("_any_%d", tmp);
                }
                else if (strcmp(method, "find") == 0 && call->block &&
                         PM_NODE_TYPE(call->block) == PM_BLOCK_NODE) {
                    pm_block_node_t *blk = (pm_block_node_t *)call->block;
                    char *bpname = extract_block_param(ctx, blk);
                    int tmp = ctx->temp_counter++;
                    emit(ctx, "mrb_int _find_%d = 0;\n", tmp);
                    emit(ctx, "for (mrb_int _fi_%d = 0; _fi_%d < sp_IntArray_length(%s); _fi_%d++) {\n", tmp, tmp, recv, tmp);
                    ctx->indent++;
                    if (bpname) {
                        char *cn = make_cname(bpname, false);
                        emit(ctx, "mrb_int %s = sp_IntArray_get(%s, _fi_%d);\n", cn, recv, tmp);
                        free(cn);
                    }
                    if (blk->body) {
                        pm_statements_node_t *stmts = (pm_statements_node_t *)blk->body;
                        if (stmts->body.size > 0) {
                            char *cond = codegen_expr(ctx, stmts->body.nodes[stmts->body.size - 1]);
                            emit(ctx, "if (%s) { _find_%d = sp_IntArray_get(%s, _fi_%d); break; }\n", cond, tmp, recv, tmp);
                            free(cond);
                        }
                    }
                    ctx->indent--;
                    emit(ctx, "}\n");
                    free(bpname);
                    r = sfmt("_find_%d", tmp);
                }
                else if (strcmp(method, "filter_map") == 0 && call->block &&
                         PM_NODE_TYPE(call->block) == PM_BLOCK_NODE) {
                    pm_block_node_t *blk = (pm_block_node_t *)call->block;
                    char *bpname = extract_block_param(ctx, blk);
                    int tmp = ctx->temp_counter++;
                    ctx->needs_gc = true;
                    emit(ctx, "sp_IntArray *_fm_%d = sp_IntArray_new();\n", tmp);
                    emit(ctx, "for (mrb_int _fmi_%d = 0; _fmi_%d < sp_IntArray_length(%s); _fmi_%d++) {\n", tmp, tmp, recv, tmp);
                    ctx->indent++;
                    if (bpname) {
                        char *cn = make_cname(bpname, false);
                        emit(ctx, "mrb_int %s = sp_IntArray_get(%s, _fmi_%d);\n", cn, recv, tmp);
                        free(cn);
                    }
                    if (blk->body) {
                        pm_statements_node_t *stmts = (pm_statements_node_t *)blk->body;
                        if (stmts->body.size > 0) {
                            char *val = codegen_expr(ctx, stmts->body.nodes[stmts->body.size - 1]);
                            emit(ctx, "sp_IntArray_push(_fm_%d, %s);\n", tmp, val);
                            free(val);
                        }
                    }
                    ctx->indent--;
                    emit(ctx, "}\n");
                    free(bpname);
                    r = sfmt("_fm_%d", tmp);
                }
                else if (strcmp(method, "flat_map") == 0 && call->block &&
                         PM_NODE_TYPE(call->block) == PM_BLOCK_NODE) {
                    /* flat_map: block returns IntArray, flatten into single IntArray */
                    pm_block_node_t *blk = (pm_block_node_t *)call->block;
                    char *bpname = extract_block_param(ctx, blk);
                    int tmp = ctx->temp_counter++;
                    emit(ctx, "sp_IntArray *_flatm_%d = sp_IntArray_new();\n", tmp);
                    emit(ctx, "for (mrb_int _fi_%d = 0; _fi_%d < sp_IntArray_length(%s); _fi_%d++) {\n", tmp, tmp, recv, tmp);
                    ctx->indent++;
                    if (bpname) {
                        char *cn = make_cname(bpname, false);
                        emit(ctx, "mrb_int %s = sp_IntArray_get(%s, _fi_%d);\n", cn, recv, tmp);
                        free(cn);
                    }
                    if (blk->body) {
                        pm_statements_node_t *stmts = (pm_statements_node_t *)blk->body;
                        if (stmts->body.size > 0) {
                            char *val = codegen_expr(ctx, stmts->body.nodes[stmts->body.size - 1]);
                            /* Append all elements from the inner array */
                            int inner = ctx->temp_counter++;
                            emit(ctx, "{ sp_IntArray *_inner_%d = %s;\n", inner, val);
                            emit(ctx, "  for (mrb_int _j_%d = 0; _j_%d < sp_IntArray_length(_inner_%d); _j_%d++)\n",
                                 inner, inner, inner, inner);
                            emit(ctx, "    sp_IntArray_push(_flatm_%d, sp_IntArray_get(_inner_%d, _j_%d));\n",
                                 tmp, inner, inner);
                            emit(ctx, "}\n");
                            free(val);
                        }
                    }
                    ctx->indent--;
                    emit(ctx, "}\n");
                    free(bpname);
                    r = sfmt("_flatm_%d", tmp);
                }
                if (r) {
                    free(recv); free(method);
                    return r;
                }

                /* Array#map with block → new IntArray (expression context) */
                if (strcmp(method, "map") == 0 && call->block &&
                    PM_NODE_TYPE(call->block) == PM_BLOCK_NODE) {
                    pm_block_node_t *blk = (pm_block_node_t *)call->block;
                    char *bpname = extract_block_param(ctx, blk);
                    int tmp = ctx->temp_counter++;
                    emit(ctx, "sp_IntArray *_map_%d = sp_IntArray_new();\n", tmp);
                    emit(ctx, "for (mrb_int _mi_%d = 0; _mi_%d < sp_IntArray_length(%s); _mi_%d++) {\n",
                         tmp, tmp, recv, tmp);
                    ctx->indent++;
                    if (bpname) {
                        char *cn = make_cname(bpname, false);
                        emit(ctx, "mrb_int %s = sp_IntArray_get(%s, _mi_%d);\n", cn, recv, tmp);
                        free(cn);
                    }
                    /* Block body as expression */
                    char *body_expr = NULL;
                    if (blk->body) {
                        pm_node_t *body = (pm_node_t *)blk->body;
                        if (PM_NODE_TYPE(body) == PM_STATEMENTS_NODE) {
                            pm_statements_node_t *stmts = (pm_statements_node_t *)body;
                            if (stmts->body.size > 0)
                                body_expr = codegen_expr(ctx, stmts->body.nodes[stmts->body.size - 1]);
                        } else {
                            body_expr = codegen_expr(ctx, body);
                        }
                    }
                    if (body_expr) {
                        emit(ctx, "sp_IntArray_push(_map_%d, %s);\n", tmp, body_expr);
                        free(body_expr);
                    }
                    ctx->indent--;
                    emit(ctx, "}\n");
                    free(recv); free(bpname); free(method);
                    return sfmt("_map_%d", tmp);
                }

                /* Array#map! with block → in-place map (expression context) */
                if (strcmp(method, "map!") == 0 && call->block &&
                    PM_NODE_TYPE(call->block) == PM_BLOCK_NODE) {
                    pm_block_node_t *blk = (pm_block_node_t *)call->block;
                    char *bpname = extract_block_param(ctx, blk);
                    int tmp = ctx->temp_counter++;
                    emit(ctx, "for (mrb_int _mi_%d = 0; _mi_%d < sp_IntArray_length(%s); _mi_%d++) {\n",
                         tmp, tmp, recv, tmp);
                    ctx->indent++;
                    if (bpname) {
                        char *cn = make_cname(bpname, false);
                        emit(ctx, "mrb_int %s = sp_IntArray_get(%s, _mi_%d);\n", cn, recv, tmp);
                        free(cn);
                    }
                    char *body_expr = NULL;
                    if (blk->body) {
                        pm_node_t *body = (pm_node_t *)blk->body;
                        if (PM_NODE_TYPE(body) == PM_STATEMENTS_NODE) {
                            pm_statements_node_t *stmts = (pm_statements_node_t *)body;
                            if (stmts->body.size > 0)
                                body_expr = codegen_expr(ctx, stmts->body.nodes[stmts->body.size - 1]);
                        } else {
                            body_expr = codegen_expr(ctx, body);
                        }
                    }
                    if (body_expr) {
                        emit(ctx, "sp_IntArray_set(%s, _mi_%d, %s);\n", recv, tmp, body_expr);
                        free(body_expr);
                    }
                    ctx->indent--;
                    emit(ctx, "}\n");
                    char *r = xstrdup(recv);
                    free(recv); free(bpname); free(method);
                    return r;
                }

                /* Array#select with block → new IntArray (expression context) */
                if (strcmp(method, "select") == 0 && call->block &&
                    PM_NODE_TYPE(call->block) == PM_BLOCK_NODE) {
                    pm_block_node_t *blk = (pm_block_node_t *)call->block;
                    char *bpname = extract_block_param(ctx, blk);
                    int tmp = ctx->temp_counter++;
                    emit(ctx, "sp_IntArray *_sel_%d = sp_IntArray_new();\n", tmp);
                    emit(ctx, "for (mrb_int _si_%d = 0; _si_%d < sp_IntArray_length(%s); _si_%d++) {\n",
                         tmp, tmp, recv, tmp);
                    ctx->indent++;
                    if (bpname) {
                        char *cn = make_cname(bpname, false);
                        emit(ctx, "mrb_int %s = sp_IntArray_get(%s, _si_%d);\n", cn, recv, tmp);
                        free(cn);
                    }
                    /* Block body as condition */
                    char *body_expr = NULL;
                    if (blk->body) {
                        pm_node_t *body = (pm_node_t *)blk->body;
                        if (PM_NODE_TYPE(body) == PM_STATEMENTS_NODE) {
                            pm_statements_node_t *stmts = (pm_statements_node_t *)body;
                            if (stmts->body.size > 0)
                                body_expr = codegen_expr(ctx, stmts->body.nodes[stmts->body.size - 1]);
                        } else {
                            body_expr = codegen_expr(ctx, body);
                        }
                    }
                    if (body_expr) {
                        if (bpname) {
                            char *cn = make_cname(bpname, false);
                            emit(ctx, "if (%s) sp_IntArray_push(_sel_%d, %s);\n", body_expr, tmp, cn);
                            free(cn);
                        } else {
                            emit(ctx, "if (%s) sp_IntArray_push(_sel_%d, sp_IntArray_get(%s, _si_%d));\n",
                                 body_expr, tmp, recv, tmp);
                        }
                        free(body_expr);
                    }
                    ctx->indent--;
                    emit(ctx, "}\n");
                    free(recv); free(bpname); free(method);
                    return sfmt("_sel_%d", tmp);
                }

                /* Array#reject with block → new IntArray (opposite of select) */
                if (strcmp(method, "reject") == 0 && call->block &&
                    PM_NODE_TYPE(call->block) == PM_BLOCK_NODE) {
                    pm_block_node_t *blk = (pm_block_node_t *)call->block;
                    char *bpname = extract_block_param(ctx, blk);
                    int tmp = ctx->temp_counter++;
                    emit(ctx, "sp_IntArray *_rej_%d = sp_IntArray_new();\n", tmp);
                    emit(ctx, "for (mrb_int _ri_%d = 0; _ri_%d < sp_IntArray_length(%s); _ri_%d++) {\n",
                         tmp, tmp, recv, tmp);
                    ctx->indent++;
                    if (bpname) {
                        char *cn = make_cname(bpname, false);
                        emit(ctx, "mrb_int %s = sp_IntArray_get(%s, _ri_%d);\n", cn, recv, tmp);
                        free(cn);
                    }
                    char *body_expr = NULL;
                    if (blk->body) {
                        pm_node_t *body = (pm_node_t *)blk->body;
                        if (PM_NODE_TYPE(body) == PM_STATEMENTS_NODE) {
                            pm_statements_node_t *stmts = (pm_statements_node_t *)body;
                            if (stmts->body.size > 0)
                                body_expr = codegen_expr(ctx, stmts->body.nodes[stmts->body.size - 1]);
                        } else {
                            body_expr = codegen_expr(ctx, body);
                        }
                    }
                    if (body_expr) {
                        if (bpname) {
                            char *cn = make_cname(bpname, false);
                            emit(ctx, "if (!(%s)) sp_IntArray_push(_rej_%d, %s);\n", body_expr, tmp, cn);
                            free(cn);
                        } else {
                            emit(ctx, "if (!(%s)) sp_IntArray_push(_rej_%d, sp_IntArray_get(%s, _ri_%d));\n",
                                 body_expr, tmp, recv, tmp);
                        }
                        free(body_expr);
                    }
                    ctx->indent--;
                    emit(ctx, "}\n");
                    free(recv); free(bpname); free(method);
                    return sfmt("_rej_%d", tmp);
                }

                /* Array#reduce/inject with initial value and block */
                if ((strcmp(method, "reduce") == 0 || strcmp(method, "inject") == 0) &&
                    call->arguments && call->arguments->arguments.size == 1 &&
                    call->block && PM_NODE_TYPE(call->block) == PM_BLOCK_NODE) {
                    pm_block_node_t *blk = (pm_block_node_t *)call->block;
                    char *init_val = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
                    /* Get block params: |acc, x| */
                    char *acc_name = NULL, *elem_name = NULL;
                    if (blk->parameters && PM_NODE_TYPE(blk->parameters) == PM_BLOCK_PARAMETERS_NODE) {
                        pm_block_parameters_node_t *bp = (pm_block_parameters_node_t *)blk->parameters;
                        if (bp->parameters && bp->parameters->requireds.size >= 2) {
                            pm_node_t *p0 = bp->parameters->requireds.nodes[0];
                            pm_node_t *p1 = bp->parameters->requireds.nodes[1];
                            if (PM_NODE_TYPE(p0) == PM_REQUIRED_PARAMETER_NODE)
                                acc_name = cstr(ctx, ((pm_required_parameter_node_t *)p0)->name);
                            if (PM_NODE_TYPE(p1) == PM_REQUIRED_PARAMETER_NODE)
                                elem_name = cstr(ctx, ((pm_required_parameter_node_t *)p1)->name);
                        }
                    }
                    int tmp = ctx->temp_counter++;
                    if (acc_name && elem_name) {
                        char *acc_cn = make_cname(acc_name, false);
                        char *elem_cn = make_cname(elem_name, false);
                        /* Register block params for type inference */
                        int sv = ctx->var_count;
                        var_declare(ctx, acc_name, vt_prim(SPINEL_TYPE_INTEGER), false);
                        var_declare(ctx, elem_name, vt_prim(SPINEL_TYPE_INTEGER), false);
                        emit(ctx, "mrb_int _red_%d; { mrb_int %s = %s;\n", tmp, acc_cn, init_val);
                        emit(ctx, "  for (mrb_int _ri_%d = 0; _ri_%d < sp_IntArray_length(%s); _ri_%d++) {\n",
                             tmp, tmp, recv, tmp);
                        emit(ctx, "    mrb_int %s = sp_IntArray_get(%s, _ri_%d);\n", elem_cn, recv, tmp);
                        /* Block body expression */
                        if (blk->body) {
                            pm_node_t *body = (pm_node_t *)blk->body;
                            char *body_expr;
                            if (PM_NODE_TYPE(body) == PM_STATEMENTS_NODE) {
                                pm_statements_node_t *stmts = (pm_statements_node_t *)body;
                                body_expr = stmts->body.size > 0 ? codegen_expr(ctx, stmts->body.nodes[stmts->body.size - 1]) : xstrdup("0");
                            } else {
                                body_expr = codegen_expr(ctx, body);
                            }
                            emit(ctx, "    %s = %s;\n", acc_cn, body_expr);
                            free(body_expr);
                        }
                        emit(ctx, "  } _red_%d = %s; }\n", tmp, acc_cn);
                        ctx->var_count = sv; /* restore var table */
                        free(acc_cn); free(elem_cn);
                    }
                    free(acc_name); free(elem_name); free(init_val);
                    free(recv); free(method);
                    return sfmt("_red_%d", tmp);
                }

                /* Array#count with block → count matching elements */
                if (strcmp(method, "count") == 0 && call->block &&
                    PM_NODE_TYPE(call->block) == PM_BLOCK_NODE) {
                    pm_block_node_t *blk = (pm_block_node_t *)call->block;
                    char *bpname = extract_block_param(ctx, blk);
                    int tmp = ctx->temp_counter++;
                    emit(ctx, "mrb_int _cnt_%d = 0;\n", tmp);
                    emit(ctx, "for (mrb_int _ci_%d = 0; _ci_%d < sp_IntArray_length(%s); _ci_%d++) {\n", tmp, tmp, recv, tmp);
                    ctx->indent++;
                    if (bpname) {
                        char *cn = make_cname(bpname, false);
                        emit(ctx, "mrb_int %s = sp_IntArray_get(%s, _ci_%d);\n", cn, recv, tmp);
                        free(cn);
                    }
                    if (blk->body) {
                        pm_statements_node_t *stmts = (pm_statements_node_t *)blk->body;
                        if (stmts->body.size > 0) {
                            char *cond = codegen_expr(ctx, stmts->body.nodes[stmts->body.size - 1]);
                            emit(ctx, "if (%s) _cnt_%d++;\n", cond, tmp);
                            free(cond);
                        }
                    }
                    ctx->indent--;
                    emit(ctx, "}\n");
                    free(bpname);
                    free(recv); free(method);
                    return sfmt("_cnt_%d", tmp);
                }
                /* Array#count without block → length */
                if (strcmp(method, "count") == 0 && !call->block) {
                    char *r = sfmt("sp_IntArray_length(%s)", recv);
                    free(recv); free(method);
                    return r;
                }

                /* Array#min_by / max_by with block */
                if ((strcmp(method, "min_by") == 0 || strcmp(method, "max_by") == 0) &&
                    call->block && PM_NODE_TYPE(call->block) == PM_BLOCK_NODE) {
                    bool is_min = (strcmp(method, "min_by") == 0);
                    pm_block_node_t *blk = (pm_block_node_t *)call->block;
                    char *bpname = extract_block_param(ctx, blk);
                    int tmp = ctx->temp_counter++;
                    emit(ctx, "mrb_int _mb_%d = 0; mrb_int _mbv_%d = 0;\n", tmp, tmp);
                    emit(ctx, "for (mrb_int _mi_%d = 0; _mi_%d < sp_IntArray_length(%s); _mi_%d++) {\n", tmp, tmp, recv, tmp);
                    ctx->indent++;
                    char *cn = bpname ? make_cname(bpname, false) : xstrdup("_x");
                    emit(ctx, "mrb_int %s = sp_IntArray_get(%s, _mi_%d);\n", cn, recv, tmp);
                    if (blk->body) {
                        pm_statements_node_t *stmts = (pm_statements_node_t *)blk->body;
                        if (stmts->body.size > 0) {
                            char *val = codegen_expr(ctx, stmts->body.nodes[stmts->body.size - 1]);
                            emit(ctx, "mrb_int _k_%d = %s;\n", tmp, val);
                            emit(ctx, "if (_mi_%d == 0 || _k_%d %s _mbv_%d) { _mbv_%d = _k_%d; _mb_%d = %s; }\n",
                                 tmp, tmp, is_min ? "<" : ">", tmp, tmp, tmp, tmp, cn);
                            free(val);
                        }
                    }
                    free(cn);
                    ctx->indent--;
                    emit(ctx, "}\n");
                    free(bpname);
                    free(recv); free(method);
                    return sfmt("_mb_%d", tmp);
                }

                /* Array#sort_by with block → copy + qsort */
                if (strcmp(method, "sort_by") == 0 && call->block &&
                    PM_NODE_TYPE(call->block) == PM_BLOCK_NODE) {
                    pm_block_node_t *blk = (pm_block_node_t *)call->block;
                    char *bpname = extract_block_param(ctx, blk);
                    /* For IntArray sort_by, we build a key array, sort by key, reorder */
                    int tmp = ctx->temp_counter++;
                    emit(ctx, "sp_IntArray *_sb_%d = sp_IntArray_dup(%s);\n", tmp, recv);
                    emit(ctx, "{ mrb_int _n_%d = sp_IntArray_length(_sb_%d);\n", tmp, tmp);
                    emit(ctx, "  mrb_int *_keys_%d = (mrb_int *)malloc(sizeof(mrb_int) * _n_%d);\n", tmp, tmp);
                    emit(ctx, "  for (mrb_int _si_%d = 0; _si_%d < _n_%d; _si_%d++) {\n", tmp, tmp, tmp, tmp);
                    char *cn = bpname ? make_cname(bpname, false) : xstrdup("_x");
                    emit(ctx, "    mrb_int %s = sp_IntArray_get(_sb_%d, _si_%d);\n", cn, tmp, tmp);
                    if (blk->body) {
                        pm_statements_node_t *stmts = (pm_statements_node_t *)blk->body;
                        if (stmts->body.size > 0) {
                            char *key = codegen_expr(ctx, stmts->body.nodes[stmts->body.size - 1]);
                            emit(ctx, "    _keys_%d[_si_%d] = %s;\n", tmp, tmp, key);
                            free(key);
                        }
                    }
                    emit(ctx, "  }\n");
                    /* Simple insertion sort by key */
                    emit(ctx, "  for (mrb_int _i_%d = 1; _i_%d < _n_%d; _i_%d++) {\n", tmp, tmp, tmp, tmp);
                    emit(ctx, "    mrb_int _kv_%d = _keys_%d[_i_%d]; mrb_int _dv_%d = _sb_%d->data[_sb_%d->start + _i_%d];\n",
                         tmp, tmp, tmp, tmp, tmp, tmp, tmp);
                    emit(ctx, "    mrb_int _j_%d = _i_%d - 1;\n", tmp, tmp);
                    emit(ctx, "    while (_j_%d >= 0 && _keys_%d[_j_%d] > _kv_%d) {\n", tmp, tmp, tmp, tmp);
                    emit(ctx, "      _keys_%d[_j_%d+1] = _keys_%d[_j_%d];\n", tmp, tmp, tmp, tmp);
                    emit(ctx, "      _sb_%d->data[_sb_%d->start + _j_%d+1] = _sb_%d->data[_sb_%d->start + _j_%d];\n",
                         tmp, tmp, tmp, tmp, tmp, tmp);
                    emit(ctx, "      _j_%d--;\n", tmp);
                    emit(ctx, "    }\n");
                    emit(ctx, "    _keys_%d[_j_%d+1] = _kv_%d; _sb_%d->data[_sb_%d->start + _j_%d+1] = _dv_%d;\n",
                         tmp, tmp, tmp, tmp, tmp, tmp, tmp);
                    emit(ctx, "  }\n");
                    emit(ctx, "  free(_keys_%d);\n", tmp);
                    emit(ctx, "}\n");
                    free(cn); free(bpname);
                    free(recv); free(method);
                    return sfmt("_sb_%d", tmp);
                }

                /* Array#sort_by! with block → in-place sort by key */
                if (strcmp(method, "sort_by!") == 0 && call->block &&
                    PM_NODE_TYPE(call->block) == PM_BLOCK_NODE) {
                    pm_block_node_t *blk = (pm_block_node_t *)call->block;
                    char *bpname = extract_block_param(ctx, blk);
                    int tmp = ctx->temp_counter++;
                    emit(ctx, "{ mrb_int _n_%d = sp_IntArray_length(%s);\n", tmp, recv);
                    emit(ctx, "  mrb_int *_keys_%d = (mrb_int *)malloc(sizeof(mrb_int) * _n_%d);\n", tmp, tmp);
                    emit(ctx, "  for (mrb_int _si_%d = 0; _si_%d < _n_%d; _si_%d++) {\n", tmp, tmp, tmp, tmp);
                    char *cn = bpname ? make_cname(bpname, false) : xstrdup("_x");
                    emit(ctx, "    mrb_int %s = sp_IntArray_get(%s, _si_%d);\n", cn, recv, tmp);
                    if (blk->body) {
                        pm_statements_node_t *stmts = (pm_statements_node_t *)blk->body;
                        if (stmts->body.size > 0) {
                            char *key = codegen_expr(ctx, stmts->body.nodes[stmts->body.size - 1]);
                            emit(ctx, "    _keys_%d[_si_%d] = %s;\n", tmp, tmp, key);
                            free(key);
                        }
                    }
                    emit(ctx, "  }\n");
                    /* Insertion sort by key, modifying original array in place */
                    emit(ctx, "  for (mrb_int _i_%d = 1; _i_%d < _n_%d; _i_%d++) {\n", tmp, tmp, tmp, tmp);
                    emit(ctx, "    mrb_int _kv_%d = _keys_%d[_i_%d]; mrb_int _dv_%d = %s->data[%s->start + _i_%d];\n",
                         tmp, tmp, tmp, tmp, recv, recv, tmp);
                    emit(ctx, "    mrb_int _j_%d = _i_%d - 1;\n", tmp, tmp);
                    emit(ctx, "    while (_j_%d >= 0 && _keys_%d[_j_%d] > _kv_%d) {\n", tmp, tmp, tmp, tmp);
                    emit(ctx, "      _keys_%d[_j_%d+1] = _keys_%d[_j_%d];\n", tmp, tmp, tmp, tmp);
                    emit(ctx, "      %s->data[%s->start + _j_%d+1] = %s->data[%s->start + _j_%d];\n",
                         recv, recv, tmp, recv, recv, tmp);
                    emit(ctx, "      _j_%d--;\n", tmp);
                    emit(ctx, "    }\n");
                    emit(ctx, "    _keys_%d[_j_%d+1] = _kv_%d; %s->data[%s->start + _j_%d+1] = _dv_%d;\n",
                         tmp, tmp, tmp, recv, recv, tmp, tmp);
                    emit(ctx, "  }\n");
                    emit(ctx, "  free(_keys_%d);\n", tmp);
                    emit(ctx, "}\n");
                    free(cn); free(bpname);
                    free(method);
                    return recv;  /* return original recv (in-place, returns self) */
                }

                /* Array#zip(other) → sp_RbArray of 2-element sp_RbArray pairs */
                if (strcmp(method, "zip") == 0 && call->arguments &&
                    call->arguments->arguments.size == 1) {
                    char *arg = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
                    ctx->needs_rb_array = true;
                    ctx->needs_poly = true;
                    int tmp = ctx->temp_counter++;
                    emit(ctx, "sp_RbArray *_zip_%d = sp_RbArray_new();\n", tmp);
                    emit(ctx, "{ mrb_int _za_%d = sp_IntArray_length(%s);\n", tmp, recv);
                    emit(ctx, "  mrb_int _zb_%d = sp_IntArray_length(%s);\n", tmp, arg);
                    emit(ctx, "  for (mrb_int _zi_%d = 0; _zi_%d < _za_%d; _zi_%d++) {\n",
                         tmp, tmp, tmp, tmp);
                    emit(ctx, "    sp_RbArray *_zp_%d = sp_RbArray_new();\n", tmp);
                    emit(ctx, "    sp_RbArray_push(_zp_%d, sp_box_int(sp_IntArray_get(%s, _zi_%d)));\n",
                         tmp, recv, tmp);
                    emit(ctx, "    sp_RbArray_push(_zp_%d, _zi_%d < _zb_%d ? sp_box_int(sp_IntArray_get(%s, _zi_%d)) : sp_box_nil());\n",
                         tmp, tmp, tmp, arg, tmp);
                    emit(ctx, "    sp_RbArray_push(_zip_%d, sp_box_obj(SP_T_OBJECT, _zp_%d));\n",
                         tmp, tmp);
                    emit(ctx, "  }\n");
                    emit(ctx, "}\n");
                    free(arg);
                    free(recv); free(method);
                    return sfmt("_zip_%d", tmp);
                }

                free(recv);
            }

            /* sp_FloatArray method calls */
            if (recv_t.kind == SPINEL_TYPE_FLOAT_ARRAY) {
                char *recv = codegen_expr(ctx, call->receiver);
                char *r = NULL;
                if (strcmp(method, "length") == 0 || strcmp(method, "size") == 0)
                    r = sfmt("sp_FloatArray_length(%s)", recv);
                else if (strcmp(method, "dup") == 0)
                    r = sfmt("sp_FloatArray_dup(%s)", recv);
                else if (strcmp(method, "push") == 0 && call->arguments &&
                         call->arguments->arguments.size == 1) {
                    char *arg = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
                    emit(ctx, "sp_FloatArray_push(%s, %s);\n", recv, arg);
                    r = sfmt("%s", recv);
                    free(arg);
                }
                if (r) { free(recv); free(method); return r; }
                free(recv);
            }

            /* sp_RbArray method calls */
            if (recv_t.kind == SPINEL_TYPE_RB_ARRAY) {
                char *recv = codegen_expr(ctx, call->receiver);
                char *r = NULL;
                if (strcmp(method, "length") == 0 || strcmp(method, "size") == 0)
                    r = sfmt("sp_RbArray_length(%s)", recv);
                else if (strcmp(method, "[]") == 0 && call->arguments &&
                         call->arguments->arguments.size == 1) {
                    char *idx = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
                    r = sfmt("sp_RbArray_get(%s, %s)", recv, idx);
                    free(idx);
                }
                else if (strcmp(method, "to_h") == 0) {
                    /* RbArray#to_h → sp_RbHash (each element is a 2-element RbArray pair) */
                    ctx->needs_rb_hash = true;
                    int tmp = ctx->temp_counter++;
                    emit(ctx, "sp_RbHash *_toh_%d = sp_RbHash_new();\n", tmp);
                    emit(ctx, "for (mrb_int _ti_%d = 0; _ti_%d < sp_RbArray_length(%s); _ti_%d++) {\n",
                         tmp, tmp, recv, tmp);
                    ctx->indent++;
                    emit(ctx, "sp_RbValue _pair_%d = sp_RbArray_get(%s, _ti_%d);\n", tmp, recv, tmp);
                    emit(ctx, "sp_RbArray *_p_%d = (sp_RbArray *)sp_unbox_obj(_pair_%d);\n", tmp, tmp);
                    emit(ctx, "const char *_pk_%d = sp_unbox_str(sp_RbArray_get(_p_%d, 0));\n", tmp, tmp);
                    emit(ctx, "sp_RbValue _pv_%d = sp_RbArray_get(_p_%d, 1);\n", tmp, tmp);
                    emit(ctx, "sp_RbHash_set(_toh_%d, _pk_%d, _pv_%d);\n", tmp, tmp, tmp);
                    ctx->indent--;
                    emit(ctx, "}\n");
                    r = sfmt("_toh_%d", tmp);
                }
                if (r) {
                    free(recv); free(method);
                    return r;
                }
                free(recv);
            }

            /* sp_StrIntHash method calls */
            if (recv_t.kind == SPINEL_TYPE_HASH) {
                char *recv = codegen_expr(ctx, call->receiver);
                char *r = NULL;
                if (strcmp(method, "[]") == 0 && call->arguments &&
                    call->arguments->arguments.size == 1) {
                    char *key = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
                    r = sfmt("sp_StrIntHash_get(%s, %s)", recv, key);
                    free(key);
                }
                else if (strcmp(method, "[]=") == 0 && call->arguments &&
                         call->arguments->arguments.size == 2) {
                    char *key = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
                    char *val = codegen_expr(ctx, call->arguments->arguments.nodes[1]);
                    r = sfmt("sp_StrIntHash_set(%s, %s, %s)", recv, key, val);
                    free(key); free(val);
                }
                else if (strcmp(method, "length") == 0)
                    r = sfmt("sp_StrIntHash_length(%s)", recv);
                else if (strcmp(method, "has_key?") == 0 && call->arguments &&
                         call->arguments->arguments.size == 1) {
                    char *key = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
                    r = sfmt("sp_StrIntHash_has_key(%s, %s)", recv, key);
                    free(key);
                }
                else if (strcmp(method, "delete") == 0 && call->arguments &&
                         call->arguments->arguments.size == 1) {
                    char *key = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
                    r = sfmt("sp_StrIntHash_delete(%s, %s)", recv, key);
                    free(key);
                }
                else if (strcmp(method, "key?") == 0 && call->arguments &&
                         call->arguments->arguments.size == 1) {
                    char *key = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
                    r = sfmt("sp_StrIntHash_has_key(%s, %s)", recv, key);
                    free(key);
                }
                else if (strcmp(method, "keys") == 0) {
                    /* h.keys returns a temporary — but typically used as h.keys.length
                     * which chains to sp_IntArray_length. Handle keys as expression. */
                    r = sfmt("sp_StrIntHash_keys(%s)", recv);
                }
                else if (strcmp(method, "values") == 0) {
                    r = sfmt("sp_StrIntHash_values(%s)", recv);
                }
                else if (strcmp(method, "merge") == 0 && call->arguments &&
                         call->arguments->arguments.size == 1) {
                    char *arg = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
                    r = sfmt("sp_StrIntHash_merge(%s, %s)", recv, arg);
                    free(arg);
                }
                else if (strcmp(method, "transform_values") == 0 && call->block &&
                         PM_NODE_TYPE(call->block) == PM_BLOCK_NODE) {
                    /* Hash#transform_values { |v| expr } → new sp_StrIntHash */
                    pm_block_node_t *blk = (pm_block_node_t *)call->block;
                    char *bpname = extract_block_param(ctx, blk);
                    int tmp = ctx->temp_counter++;
                    emit(ctx, "sp_StrIntHash *_tv_%d = sp_StrIntHash_new();\n", tmp);
                    emit(ctx, "for (sp_HashEntry *_tve_%d = %s->first; _tve_%d; _tve_%d = _tve_%d->order_next) {\n",
                         tmp, recv, tmp, tmp, tmp);
                    ctx->indent++;
                    if (bpname) {
                        char *cn = make_cname(bpname, false);
                        emit(ctx, "mrb_int %s = _tve_%d->value;\n", cn, tmp);
                        free(cn);
                    }
                    char *body_expr = NULL;
                    if (blk->body) {
                        pm_node_t *body = (pm_node_t *)blk->body;
                        if (PM_NODE_TYPE(body) == PM_STATEMENTS_NODE) {
                            pm_statements_node_t *stmts = (pm_statements_node_t *)body;
                            if (stmts->body.size > 0)
                                body_expr = codegen_expr(ctx, stmts->body.nodes[stmts->body.size - 1]);
                        } else {
                            body_expr = codegen_expr(ctx, body);
                        }
                    }
                    if (body_expr) {
                        emit(ctx, "sp_StrIntHash_set(_tv_%d, _tve_%d->key, %s);\n", tmp, tmp, body_expr);
                        free(body_expr);
                    }
                    ctx->indent--;
                    emit(ctx, "}\n");
                    free(bpname);
                    r = sfmt("_tv_%d", tmp);
                }
                if (r) {
                    free(recv); free(method);
                    return r;
                }
                free(recv);
            }

            /* sp_RbHash method calls (heterogeneous hash) */
            if (recv_t.kind == SPINEL_TYPE_RB_HASH) {
                char *recv = codegen_expr(ctx, call->receiver);
                char *r = NULL;
                if (strcmp(method, "[]") == 0 && call->arguments &&
                    call->arguments->arguments.size == 1) {
                    char *key = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
                    r = sfmt("sp_RbHash_get(%s, %s)", recv, key);
                    free(key);
                }
                else if (strcmp(method, "[]=") == 0 && call->arguments &&
                         call->arguments->arguments.size == 2) {
                    char *key = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
                    char *val = codegen_expr(ctx, call->arguments->arguments.nodes[1]);
                    vtype_t val_t = infer_type(ctx, call->arguments->arguments.nodes[1]);
                    char *boxed = poly_box_expr_vt(ctx, val_t, val);
                    r = sfmt("sp_RbHash_set(%s, %s, %s)", recv, key, boxed);
                    free(key); free(val); free(boxed);
                }
                else if (strcmp(method, "length") == 0 || strcmp(method, "size") == 0)
                    r = sfmt("sp_RbHash_length(%s)", recv);
                else if (strcmp(method, "has_key?") == 0 && call->arguments &&
                         call->arguments->arguments.size == 1) {
                    char *key = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
                    r = sfmt("sp_RbHash_has_key(%s, %s)", recv, key);
                    free(key);
                }
                else if (strcmp(method, "key?") == 0 && call->arguments &&
                         call->arguments->arguments.size == 1) {
                    char *key = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
                    r = sfmt("sp_RbHash_has_key(%s, %s)", recv, key);
                    free(key);
                }
                else if (strcmp(method, "merge") == 0 && call->arguments &&
                         call->arguments->arguments.size == 1) {
                    char *arg = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
                    r = sfmt("sp_RbHash_merge(%s, %s)", recv, arg);
                    free(arg);
                }
                else if (strcmp(method, "transform_values") == 0 && call->block &&
                         PM_NODE_TYPE(call->block) == PM_BLOCK_NODE) {
                    /* RbHash#transform_values { |v| expr } → new sp_RbHash */
                    pm_block_node_t *blk = (pm_block_node_t *)call->block;
                    char *bpname = extract_block_param(ctx, blk);
                    int tmp = ctx->temp_counter++;
                    /* Temporarily register block param as POLY */
                    int saved_vc = ctx->var_count;
                    vtype_t saved_type = {0};
                    var_entry_t *existing_v = bpname ? var_lookup(ctx, bpname) : NULL;
                    if (existing_v) {
                        saved_type = existing_v->type;
                        existing_v->type = vt_prim(SPINEL_TYPE_POLY);
                    } else if (bpname) {
                        assert(ctx->var_count < MAX_VARS);
                        var_entry_t *nv = &ctx->vars[ctx->var_count++];
                        snprintf(nv->name, sizeof(nv->name), "%s", bpname);
                        nv->type = vt_prim(SPINEL_TYPE_POLY);
                        nv->declared = false;
                        nv->is_constant = false;
                    }
                    emit(ctx, "sp_RbHash *_tv_%d = sp_RbHash_new();\n", tmp);
                    emit(ctx, "for (sp_RbHashEntry *_tve_%d = %s->first; _tve_%d; _tve_%d = _tve_%d->order_next) {\n",
                         tmp, recv, tmp, tmp, tmp);
                    ctx->indent++;
                    if (bpname) {
                        char *cn = make_cname(bpname, false);
                        emit(ctx, "sp_RbValue %s = _tve_%d->value;\n", cn, tmp);
                        free(cn);
                    }
                    char *body_expr = NULL;
                    if (blk->body) {
                        pm_node_t *body = (pm_node_t *)blk->body;
                        if (PM_NODE_TYPE(body) == PM_STATEMENTS_NODE) {
                            pm_statements_node_t *stmts = (pm_statements_node_t *)body;
                            if (stmts->body.size > 0)
                                body_expr = codegen_expr(ctx, stmts->body.nodes[stmts->body.size - 1]);
                        } else {
                            body_expr = codegen_expr(ctx, body);
                        }
                    }
                    if (body_expr) {
                        vtype_t body_t = blk->body ? infer_type(ctx, (pm_node_t *)blk->body) : vt_prim(SPINEL_TYPE_NIL);
                        char *boxed = poly_box_expr_vt(ctx, body_t, body_expr);
                        emit(ctx, "sp_RbHash_set(_tv_%d, _tve_%d->key, %s);\n", tmp, tmp, boxed);
                        free(body_expr); free(boxed);
                    }
                    ctx->indent--;
                    emit(ctx, "}\n");
                    /* Restore var table */
                    if (existing_v) {
                        existing_v->type = saved_type;
                    } else {
                        ctx->var_count = saved_vc;
                    }
                    free(bpname);
                    r = sfmt("_tv_%d", tmp);
                }
                if (r) {
                    free(recv); free(method);
                    return r;
                }
                free(recv);
            }

            /* sp_Time method calls */
            if (recv_t.kind == SPINEL_TYPE_TIME) {
                char *recv = codegen_expr(ctx, call->receiver);
                char *r = NULL;
                if (strcmp(method, "to_i") == 0)
                    r = sfmt("sp_Time_to_i(%s)", recv);
                else if (strcmp(method, "-") == 0 && call->arguments &&
                         call->arguments->arguments.size == 1) {
                    char *arg = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
                    r = sfmt("sp_Time_diff(%s, %s)", recv, arg);
                    free(arg);
                }
                if (r) {
                    free(recv); free(method);
                    return r;
                }
                free(recv);
            }

            /* sp_File instance method calls */
            if (recv_t.kind == SPINEL_TYPE_FILE) {
                char *recv = codegen_expr(ctx, call->receiver);
                char *r = NULL;
                if (strcmp(method, "readline") == 0)
                    r = sfmt("sp_File_readline(%s)", recv);
                else if (strcmp(method, "read") == 0)
                    r = sfmt("sp_File_read_all(%s)", recv);
                else if (strcmp(method, "puts") == 0 && call->arguments &&
                         call->arguments->arguments.size > 0) {
                    char *arg = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
                    r = sfmt("(sp_File_puts(%s, %s), (mrb_int)0)", recv, arg);
                    free(arg);
                } else if (strcmp(method, "write") == 0 && call->arguments &&
                           call->arguments->arguments.size > 0) {
                    char *arg = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
                    r = sfmt("(sp_File_write_str(%s, %s), (mrb_int)0)", recv, arg);
                    free(arg);
                }
                if (r) {
                    free(recv); free(method);
                    return r;
                }
                free(recv);
            }

            /* sp_Range method calls */
            if (recv_t.kind == SPINEL_TYPE_RANGE) {
                char *recv = codegen_expr(ctx, call->receiver);
                char *r = NULL;
                if (strcmp(method, "first") == 0)
                    r = sfmt("(%s).first", recv);
                else if (strcmp(method, "last") == 0)
                    r = sfmt("(%s).last", recv);
                else if (strcmp(method, "include?") == 0 && call->arguments &&
                         call->arguments->arguments.size == 1) {
                    char *arg = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
                    r = sfmt("sp_Range_include_p(%s, %s)", recv, arg);
                    free(arg);
                }
                else if (strcmp(method, "to_a") == 0)
                    r = sfmt("sp_Range_to_a(%s)", recv);
                else if (strcmp(method, "sum") == 0 && call->block &&
                         PM_NODE_TYPE(call->block) == PM_BLOCK_NODE) {
                    /* Range#sum { |x| expr } → accumulate block results */
                    pm_block_node_t *blk = (pm_block_node_t *)call->block;
                    char *bpname = extract_block_param(ctx, blk);
                    int tmp = ctx->temp_counter++;
                    char *cn = bpname ? make_cname(bpname, false) : xstrdup("_ri");
                    emit(ctx, "mrb_int _rsum_%d = 0;\n", tmp);
                    emit(ctx, "for (mrb_int %s = (%s).first; %s <= (%s).last; %s++) {\n", cn, recv, cn, recv, cn);
                    ctx->indent++;
                    if (blk->body) {
                        pm_statements_node_t *stmts = (pm_statements_node_t *)blk->body;
                        if (stmts->body.size > 0) {
                            char *val = codegen_expr(ctx, stmts->body.nodes[stmts->body.size - 1]);
                            emit(ctx, "_rsum_%d += %s;\n", tmp, val);
                            free(val);
                        }
                    }
                    ctx->indent--;
                    emit(ctx, "}\n");
                    free(cn); free(bpname);
                    r = sfmt("_rsum_%d", tmp);
                }
                else if (strcmp(method, "sum") == 0) {
                    int tmp = ctx->temp_counter++;
                    emit(ctx, "mrb_int _rsum_%d = 0; { mrb_int _ri_%d;\n", tmp, tmp);
                    emit(ctx, "  for (_ri_%d = (%s).first; _ri_%d <= (%s).last; _ri_%d++)\n", tmp, recv, tmp, recv, tmp);
                    emit(ctx, "    _rsum_%d += _ri_%d;\n", tmp, tmp);
                    emit(ctx, "}\n");
                    r = sfmt("_rsum_%d", tmp);
                }
                else if (strcmp(method, "count") == 0 && call->block &&
                         PM_NODE_TYPE(call->block) == PM_BLOCK_NODE) {
                    /* Range#count { |x| cond } → count matching elements */
                    pm_block_node_t *blk = (pm_block_node_t *)call->block;
                    char *bpname = extract_block_param(ctx, blk);
                    int tmp = ctx->temp_counter++;
                    char *cn = bpname ? make_cname(bpname, false) : xstrdup("_ri");
                    emit(ctx, "mrb_int _rcnt_%d = 0;\n", tmp);
                    emit(ctx, "for (mrb_int %s = (%s).first; %s <= (%s).last; %s++) {\n", cn, recv, cn, recv, cn);
                    ctx->indent++;
                    if (blk->body) {
                        pm_statements_node_t *stmts = (pm_statements_node_t *)blk->body;
                        if (stmts->body.size > 0) {
                            char *cond = codegen_expr(ctx, stmts->body.nodes[stmts->body.size - 1]);
                            emit(ctx, "if (%s) _rcnt_%d++;\n", cond, tmp);
                            free(cond);
                        }
                    }
                    ctx->indent--;
                    emit(ctx, "}\n");
                    free(cn); free(bpname);
                    r = sfmt("_rcnt_%d", tmp);
                }
                else if (strcmp(method, "count") == 0)
                    r = sfmt("((%s).last - (%s).first + 1)", recv, recv);
                else if (strcmp(method, "map") == 0 && call->block &&
                         PM_NODE_TYPE(call->block) == PM_BLOCK_NODE) {
                    /* Range#map { |x| expr } → IntArray */
                    pm_block_node_t *blk = (pm_block_node_t *)call->block;
                    char *bpname = extract_block_param(ctx, blk);
                    int tmp = ctx->temp_counter++;
                    char *cn = bpname ? make_cname(bpname, false) : xstrdup("_ri");
                    emit(ctx, "sp_IntArray *_rmap_%d = sp_IntArray_new();\n", tmp);
                    emit(ctx, "for (mrb_int %s = (%s).first; %s <= (%s).last; %s++) {\n", cn, recv, cn, recv, cn);
                    ctx->indent++;
                    if (blk->body) {
                        pm_statements_node_t *stmts = (pm_statements_node_t *)blk->body;
                        if (stmts->body.size > 0) {
                            char *val = codegen_expr(ctx, stmts->body.nodes[stmts->body.size - 1]);
                            emit(ctx, "sp_IntArray_push(_rmap_%d, %s);\n", tmp, val);
                            free(val);
                        }
                    }
                    ctx->indent--;
                    emit(ctx, "}\n");
                    free(cn); free(bpname);
                    r = sfmt("_rmap_%d", tmp);
                }
                else if (strcmp(method, "length") == 0 || strcmp(method, "size") == 0)
                    r = sfmt("((%s).last - (%s).first + 1)", recv, recv);
                if (r) {
                    free(recv); free(method);
                    return r;
                }
                free(recv);
            }

            /* String method calls */
            if (recv_t.kind == SPINEL_TYPE_STRING) {
                char *recv = codegen_expr(ctx, call->receiver);
                char *r = NULL;
                if (strcmp(method, "[]") == 0 && call->arguments &&
                    call->arguments->arguments.size == 1) {
                    char *idx = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
                    r = sfmt("sp_str_char_at(%s, %s)", recv, idx);
                    free(idx);
                }
                else if (strcmp(method, "length") == 0 || strcmp(method, "size") == 0)
                    r = sfmt("((mrb_int)strlen(%s))", recv);
                else if (strcmp(method, "upcase") == 0)
                    r = sfmt("sp_str_upcase(%s)", recv);
                else if (strcmp(method, "downcase") == 0)
                    r = sfmt("sp_str_downcase(%s)", recv);
                else if (strcmp(method, "include?") == 0 && call->arguments &&
                         call->arguments->arguments.size == 1) {
                    char *arg = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
                    r = sfmt("(strstr(%s, %s) != NULL)", recv, arg);
                    free(arg);
                }
                else if (strcmp(method, "+") == 0 && call->arguments &&
                         call->arguments->arguments.size == 1) {
                    char *arg = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
                    r = sfmt("sp_str_concat(%s, %s)", recv, arg);
                    free(arg);
                }
                else if (strcmp(method, "strip") == 0)
                    r = sfmt("sp_str_strip(%s)", recv);
                else if (strcmp(method, "chomp") == 0)
                    r = sfmt("sp_str_chomp(%s)", recv);
                else if (strcmp(method, "capitalize") == 0)
                    r = sfmt("sp_str_capitalize(%s)", recv);
                else if (strcmp(method, "reverse") == 0)
                    r = sfmt("sp_str_reverse(%s)", recv);
                else if (strcmp(method, "count") == 0 && call->arguments &&
                         call->arguments->arguments.size == 1) {
                    char *arg = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
                    r = sfmt("sp_str_count(%s, %s)", recv, arg);
                    free(arg);
                }
                else if (strcmp(method, "start_with?") == 0 && call->arguments &&
                         call->arguments->arguments.size == 1) {
                    char *arg = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
                    r = sfmt("sp_str_starts_with(%s, %s)", recv, arg);
                    free(arg);
                }
                else if (strcmp(method, "end_with?") == 0 && call->arguments &&
                         call->arguments->arguments.size == 1) {
                    char *arg = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
                    r = sfmt("sp_str_ends_with(%s, %s)", recv, arg);
                    free(arg);
                }
                else if (strcmp(method, "match?") == 0 && call->arguments &&
                         call->arguments->arguments.size == 1) {
                    vtype_t argt = infer_type(ctx, call->arguments->arguments.nodes[0]);
                    if (argt.kind == SPINEL_TYPE_REGEXP) {
                        char *arg = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
                        r = sfmt("sp_re_match_p(%s, %s)", arg, recv);
                        free(arg);
                    } else {
                        char *arg = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
                        r = sfmt("(strstr(%s, %s) != NULL)", recv, arg);
                        free(arg);
                    }
                }
                else if (strcmp(method, "gsub") == 0 && call->arguments &&
                         call->arguments->arguments.size == 2) {
                    vtype_t argt = infer_type(ctx, call->arguments->arguments.nodes[0]);
                    if (argt.kind == SPINEL_TYPE_REGEXP) {
                        char *pat = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
                        char *to = codegen_expr(ctx, call->arguments->arguments.nodes[1]);
                        r = sfmt("sp_re_gsub(%s, %s, %s)", pat, recv, to);
                        free(pat); free(to);
                    } else {
                        char *from = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
                        char *to = codegen_expr(ctx, call->arguments->arguments.nodes[1]);
                        r = sfmt("sp_str_gsub(%s, %s, %s)", recv, from, to);
                        free(from); free(to);
                    }
                }
                else if (strcmp(method, "sub") == 0 && call->arguments &&
                         call->arguments->arguments.size == 2) {
                    vtype_t argt = infer_type(ctx, call->arguments->arguments.nodes[0]);
                    if (argt.kind == SPINEL_TYPE_REGEXP) {
                        char *pat = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
                        char *to = codegen_expr(ctx, call->arguments->arguments.nodes[1]);
                        r = sfmt("sp_re_sub(%s, %s, %s)", pat, recv, to);
                        free(pat); free(to);
                    } else {
                        char *from = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
                        char *to = codegen_expr(ctx, call->arguments->arguments.nodes[1]);
                        r = sfmt("sp_str_sub(%s, %s, %s)", recv, from, to);
                        free(from); free(to);
                    }
                }
                else if (strcmp(method, "split") == 0 && call->arguments &&
                         call->arguments->arguments.size == 1) {
                    vtype_t argt = infer_type(ctx, call->arguments->arguments.nodes[0]);
                    if (argt.kind == SPINEL_TYPE_REGEXP) {
                        char *pat = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
                        r = sfmt("sp_re_split(%s, %s)", pat, recv);
                        free(pat);
                    } else {
                        char *arg = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
                        r = sfmt("sp_str_split(%s, %s)", recv, arg);
                        free(arg);
                    }
                }
                else if (strcmp(method, "empty?") == 0)
                    r = sfmt("(strlen(%s) == 0)", recv);
                else if (strcmp(method, "to_i") == 0)
                    r = sfmt("((mrb_int)strtol(%s, NULL, 10))", recv);
                else if (strcmp(method, "ord") == 0)
                    r = sfmt("((mrb_int)(unsigned char)(%s)[0])", recv);
                else if (strcmp(method, "getbyte") == 0 && call->arguments &&
                         call->arguments->arguments.size == 1) {
                    char *idx = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
                    r = sfmt("((mrb_int)(unsigned char)(%s)[%s])", recv, idx);
                    free(idx);
                }
                else if (strcmp(method, "bytesize") == 0)
                    r = sfmt("((mrb_int)strlen(%s))", recv);
                else if (strcmp(method, "to_sym") == 0)
                    r = sfmt("%s", recv); /* symbols are strings in Spinel */
                else if (strcmp(method, "each_line") == 0) {
                    /* Return an sp_StrArray of lines */
                    ctx->needs_str_split = true;
                    r = sfmt("sp_str_split(%s, \"\\n\")", recv);
                }
                else if (strcmp(method, "to_f") == 0)
                    r = sfmt("sp_str_to_f(%s)", recv);
                else if (strcmp(method, "ljust") == 0 && call->arguments) {
                    char *w = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
                    if (call->arguments->arguments.size >= 2) {
                        char *pad = codegen_expr(ctx, call->arguments->arguments.nodes[1]);
                        r = sfmt("sp_str_ljust(%s, %s, (%s)[0])", recv, w, pad);
                        free(pad);
                    } else {
                        r = sfmt("sp_str_ljust(%s, %s, ' ')", recv, w);
                    }
                    free(w);
                }
                else if (strcmp(method, "rjust") == 0 && call->arguments) {
                    char *w = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
                    if (call->arguments->arguments.size >= 2) {
                        char *pad = codegen_expr(ctx, call->arguments->arguments.nodes[1]);
                        r = sfmt("sp_str_rjust(%s, %s, (%s)[0])", recv, w, pad);
                        free(pad);
                    } else {
                        r = sfmt("sp_str_rjust(%s, %s, ' ')", recv, w);
                    }
                    free(w);
                }
                else if (strcmp(method, "center") == 0 && call->arguments) {
                    char *w = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
                    if (call->arguments->arguments.size >= 2) {
                        char *pad = codegen_expr(ctx, call->arguments->arguments.nodes[1]);
                        r = sfmt("sp_str_center(%s, %s, (%s)[0])", recv, w, pad);
                        free(pad);
                    } else {
                        r = sfmt("sp_str_center(%s, %s, ' ')", recv, w);
                    }
                    free(w);
                }
                else if (strcmp(method, "lstrip") == 0)
                    r = sfmt("sp_str_lstrip(%s)", recv);
                else if (strcmp(method, "rstrip") == 0)
                    r = sfmt("sp_str_rstrip(%s)", recv);
                else if (strcmp(method, "tr") == 0 && call->arguments &&
                         call->arguments->arguments.size == 2) {
                    char *from = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
                    char *to = codegen_expr(ctx, call->arguments->arguments.nodes[1]);
                    r = sfmt("sp_str_tr(%s, %s, %s)", recv, from, to);
                    free(from); free(to);
                }
                else if (strcmp(method, "delete") == 0 && call->arguments &&
                         call->arguments->arguments.size == 1) {
                    char *arg = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
                    r = sfmt("sp_str_delete(%s, %s)", recv, arg);
                    free(arg);
                }
                else if (strcmp(method, "squeeze") == 0)
                    r = sfmt("sp_str_squeeze(%s)", recv);
                else if (strcmp(method, "chars") == 0) {
                    ctx->needs_str_split = true; /* ensures sp_StrArray is emitted */
                    r = sfmt("sp_str_chars(%s)", recv);
                }
                else if (strcmp(method, "bytes") == 0)
                    r = sfmt("sp_str_bytes(%s)", recv);
                else if (strcmp(method, "freeze") == 0)
                    r = sfmt("%s", recv); /* no-op in AOT */
                else if (strcmp(method, "frozen?") == 0)
                    r = xstrdup("TRUE"); /* all strings frozen in AOT */
                else if (strcmp(method, "to_s") == 0)
                    r = sfmt("%s", recv); /* string.to_s → identity */
                else if (strcmp(method, "dup") == 0) {
                    ctx->needs_sp_string = true;
                    r = sfmt("sp_String_new(%s)", recv); /* mutable copy */
                }
                else if (strcmp(method, "slice") == 0 && call->arguments &&
                         call->arguments->arguments.size == 2) {
                    char *start = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
                    char *len = codegen_expr(ctx, call->arguments->arguments.nodes[1]);
                    r = sfmt("sp_str_slice(%s, %s, %s)", recv, start, len);
                    free(start); free(len);
                }
                else if (strcmp(method, "hex") == 0)
                    r = sfmt("((mrb_int)strtol(%s, NULL, 16))", recv);
                else if (strcmp(method, "oct") == 0)
                    r = sfmt("((mrb_int)strtol(%s, NULL, 8))", recv);
                if (r) {
                    free(recv); free(method);
                    return r;
                }
                free(recv);
            }

            /* sp_String (mutable string) method calls */
            if (recv_t.kind == SPINEL_TYPE_SP_STRING) {
                char *recv = codegen_expr(ctx, call->receiver);
                char *r = NULL;
                if (strcmp(method, "length") == 0 || strcmp(method, "size") == 0)
                    r = sfmt("sp_String_length(%s)", recv);
                else if (strcmp(method, "upcase") == 0)
                    r = sfmt("sp_String_upcase(%s)", recv);
                else if (strcmp(method, "reverse") == 0)
                    r = sfmt("sp_String_reverse(%s)", recv);
                else if (strcmp(method, "include?") == 0 && call->arguments &&
                         call->arguments->arguments.size == 1) {
                    char *arg = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
                    r = sfmt("sp_String_include(%s, %s)", recv, arg);
                    free(arg);
                }
                else if (strcmp(method, "to_s") == 0)
                    r = sfmt("sp_String_cstr(%s)", recv);
                else if (strcmp(method, "[]") == 0 && call->arguments &&
                         call->arguments->arguments.size == 1) {
                    char *idx = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
                    r = sfmt("sp_String_char_at(%s, %s)", recv, idx);
                    free(idx);
                }
                else if (strcmp(method, "gsub") == 0 && call->arguments &&
                         call->arguments->arguments.size == 2) {
                    char *from = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
                    char *to = codegen_expr(ctx, call->arguments->arguments.nodes[1]);
                    r = sfmt("sp_str_gsub(sp_String_cstr(%s), %s, %s)", recv, from, to);
                    free(from); free(to);
                }
                else if (strcmp(method, "split") == 0 && call->arguments &&
                         call->arguments->arguments.size == 1) {
                    char *arg = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
                    r = sfmt("sp_str_split(sp_String_cstr(%s), %s)", recv, arg);
                    free(arg);
                }
                else if (strcmp(method, "+") == 0 && call->arguments &&
                         call->arguments->arguments.size == 1) {
                    char *arg = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
                    r = sfmt("sp_str_concat(sp_String_cstr(%s), %s)", recv, arg);
                    free(arg);
                }
                else if (strcmp(method, "replace") == 0 && call->arguments &&
                         call->arguments->arguments.size == 1) {
                    char *arg = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
                    r = sfmt("(sp_String_replace(%s, %s), %s)", recv, arg, recv);
                    free(arg);
                }
                else if (strcmp(method, "clear") == 0)
                    r = sfmt("(sp_String_clear(%s), %s)", recv, recv);
                /* Delegate to const char * helpers via sp_String_cstr */
                else if (strcmp(method, "downcase") == 0)
                    r = sfmt("sp_str_downcase(sp_String_cstr(%s))", recv);
                else if (strcmp(method, "strip") == 0)
                    r = sfmt("sp_str_strip(sp_String_cstr(%s))", recv);
                else if (strcmp(method, "chomp") == 0)
                    r = sfmt("sp_str_chomp(sp_String_cstr(%s))", recv);
                else if (strcmp(method, "capitalize") == 0)
                    r = sfmt("sp_str_capitalize(sp_String_cstr(%s))", recv);
                else if (strcmp(method, "start_with?") == 0 && call->arguments &&
                         call->arguments->arguments.size == 1) {
                    char *arg = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
                    r = sfmt("sp_str_starts_with(sp_String_cstr(%s), %s)", recv, arg);
                    free(arg);
                }
                else if (strcmp(method, "end_with?") == 0 && call->arguments &&
                         call->arguments->arguments.size == 1) {
                    char *arg = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
                    r = sfmt("sp_str_ends_with(sp_String_cstr(%s), %s)", recv, arg);
                    free(arg);
                }
                else if (strcmp(method, "empty?") == 0)
                    r = sfmt("(sp_String_length(%s) == 0)", recv);
                else if (strcmp(method, "sub") == 0 && call->arguments &&
                         call->arguments->arguments.size == 2) {
                    char *from = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
                    char *to = codegen_expr(ctx, call->arguments->arguments.nodes[1]);
                    r = sfmt("sp_str_sub(sp_String_cstr(%s), %s, %s)", recv, from, to);
                    free(from); free(to);
                }
                else if (strcmp(method, "ljust") == 0 && call->arguments) {
                    char *w = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
                    if (call->arguments->arguments.size >= 2) {
                        char *pad = codegen_expr(ctx, call->arguments->arguments.nodes[1]);
                        r = sfmt("sp_str_ljust(sp_String_cstr(%s), %s, (%s)[0])", recv, w, pad);
                        free(pad);
                    } else {
                        r = sfmt("sp_str_ljust(sp_String_cstr(%s), %s, ' ')", recv, w);
                    }
                    free(w);
                }
                else if (strcmp(method, "rjust") == 0 && call->arguments) {
                    char *w = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
                    if (call->arguments->arguments.size >= 2) {
                        char *pad = codegen_expr(ctx, call->arguments->arguments.nodes[1]);
                        r = sfmt("sp_str_rjust(sp_String_cstr(%s), %s, (%s)[0])", recv, w, pad);
                        free(pad);
                    } else {
                        r = sfmt("sp_str_rjust(sp_String_cstr(%s), %s, ' ')", recv, w);
                    }
                    free(w);
                }
                else if (strcmp(method, "center") == 0 && call->arguments) {
                    char *w = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
                    if (call->arguments->arguments.size >= 2) {
                        char *pad = codegen_expr(ctx, call->arguments->arguments.nodes[1]);
                        r = sfmt("sp_str_center(sp_String_cstr(%s), %s, (%s)[0])", recv, w, pad);
                        free(pad);
                    } else {
                        r = sfmt("sp_str_center(sp_String_cstr(%s), %s, ' ')", recv, w);
                    }
                    free(w);
                }
                else if (strcmp(method, "lstrip") == 0)
                    r = sfmt("sp_str_lstrip(sp_String_cstr(%s))", recv);
                else if (strcmp(method, "rstrip") == 0)
                    r = sfmt("sp_str_rstrip(sp_String_cstr(%s))", recv);
                else if (strcmp(method, "tr") == 0 && call->arguments &&
                         call->arguments->arguments.size == 2) {
                    char *from = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
                    char *to = codegen_expr(ctx, call->arguments->arguments.nodes[1]);
                    r = sfmt("sp_str_tr(sp_String_cstr(%s), %s, %s)", recv, from, to);
                    free(from); free(to);
                }
                else if (strcmp(method, "delete") == 0 && call->arguments &&
                         call->arguments->arguments.size == 1) {
                    char *arg = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
                    r = sfmt("sp_str_delete(sp_String_cstr(%s), %s)", recv, arg);
                    free(arg);
                }
                else if (strcmp(method, "squeeze") == 0)
                    r = sfmt("sp_str_squeeze(sp_String_cstr(%s))", recv);
                else if (strcmp(method, "to_i") == 0)
                    r = sfmt("((mrb_int)strtol(sp_String_cstr(%s), NULL, 10))", recv);
                else if (strcmp(method, "count") == 0 && call->arguments &&
                         call->arguments->arguments.size == 1) {
                    char *arg = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
                    r = sfmt("sp_str_count(sp_String_cstr(%s), %s)", recv, arg);
                    free(arg);
                }
                else if (strcmp(method, "each_line") == 0) {
                    ctx->needs_str_split = true;
                    r = sfmt("sp_str_split(sp_String_cstr(%s), \"\\n\")", recv);
                }
                else if (strcmp(method, "freeze") == 0)
                    r = sfmt("sp_String_cstr(%s)", recv);
                else if (strcmp(method, "frozen?") == 0)
                    r = xstrdup("FALSE"); /* mutable strings are not frozen */
                else if (strcmp(method, "chars") == 0) {
                    ctx->needs_str_split = true;
                    r = sfmt("sp_str_chars(sp_String_cstr(%s))", recv);
                }
                else if (strcmp(method, "bytes") == 0)
                    r = sfmt("sp_str_bytes(sp_String_cstr(%s))", recv);
                else if (strcmp(method, "to_f") == 0)
                    r = sfmt("sp_str_to_f(sp_String_cstr(%s))", recv);
                else if (strcmp(method, "ord") == 0)
                    r = sfmt("((mrb_int)(unsigned char)(sp_String_cstr(%s))[0])", recv);
                else if (strcmp(method, "getbyte") == 0 && call->arguments &&
                         call->arguments->arguments.size == 1) {
                    char *idx = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
                    r = sfmt("((mrb_int)(unsigned char)sp_String_cstr(%s)[%s])", recv, idx);
                    free(idx);
                }
                else if (strcmp(method, "setbyte") == 0 && call->arguments &&
                         call->arguments->arguments.size == 2) {
                    char *idx = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
                    char *val = codegen_expr(ctx, call->arguments->arguments.nodes[1]);
                    r = sfmt("(sp_String_setbyte(%s, %s, %s), %s)", recv, idx, val, val);
                    free(idx); free(val);
                }
                else if (strcmp(method, "bytesize") == 0)
                    r = sfmt("sp_String_length(%s)", recv);
                else if (strcmp(method, "match?") == 0 && call->arguments &&
                         call->arguments->arguments.size == 1) {
                    char *arg = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
                    r = sfmt("(strstr(sp_String_cstr(%s), %s) != NULL)", recv, arg);
                    free(arg);
                }
                else if (strcmp(method, "dup") == 0)
                    r = sfmt("sp_String_dup(%s)", recv);
                if (r) {
                    free(recv); free(method);
                    return r;
                }
                free(recv);
            }

            /* String array method calls */
            if (recv_t.kind == SPINEL_TYPE_STR_ARRAY) {
                char *recv = codegen_expr(ctx, call->receiver);
                char *r = NULL;
                if (strcmp(method, "length") == 0 || strcmp(method, "size") == 0)
                    r = sfmt("sp_StrArray_length(%s)", recv);
                else if (strcmp(method, "[]") == 0 && call->arguments &&
                         call->arguments->arguments.size == 1) {
                    char *idx = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
                    r = sfmt("(%s)->data[%s]", recv, idx);
                    free(idx);
                }
                else if (strcmp(method, "first") == 0)
                    r = sfmt("(%s)->data[0]", recv);
                else if (strcmp(method, "last") == 0)
                    r = sfmt("(%s)->data[sp_StrArray_length(%s) - 1]", recv, recv);
                else if (strcmp(method, "empty?") == 0)
                    r = sfmt("(sp_StrArray_length(%s) == 0)", recv);
                else if (strcmp(method, "join") == 0 && call->arguments &&
                         call->arguments->arguments.size == 1) {
                    char *sep = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
                    int tmp = ctx->temp_counter++;
                    emit(ctx, "const char *_sj_%d; { size_t _total_%d = 0; size_t _sl_%d = strlen(%s);\n", tmp, tmp, tmp, sep);
                    emit(ctx, "  for (mrb_int _ji_%d = 0; _ji_%d < sp_StrArray_length(%s); _ji_%d++) _total_%d += strlen((%s)->data[_ji_%d]) + _sl_%d;\n",
                         tmp, tmp, recv, tmp, tmp, recv, tmp, tmp);
                    emit(ctx, "  char *_jbuf_%d = (char *)malloc(_total_%d + 1); size_t _jp_%d = 0;\n", tmp, tmp, tmp);
                    emit(ctx, "  for (mrb_int _ji_%d = 0; _ji_%d < sp_StrArray_length(%s); _ji_%d++) {\n", tmp, tmp, recv, tmp);
                    emit(ctx, "    if (_ji_%d > 0) { memcpy(_jbuf_%d + _jp_%d, %s, _sl_%d); _jp_%d += _sl_%d; }\n", tmp, tmp, tmp, sep, tmp, tmp, tmp);
                    emit(ctx, "    size_t _el_%d = strlen((%s)->data[_ji_%d]); memcpy(_jbuf_%d + _jp_%d, (%s)->data[_ji_%d], _el_%d); _jp_%d += _el_%d;\n",
                         tmp, recv, tmp, tmp, tmp, recv, tmp, tmp, tmp, tmp);
                    emit(ctx, "  } _jbuf_%d[_jp_%d] = '\\0'; _sj_%d = _jbuf_%d; }\n", tmp, tmp, tmp, tmp);
                    free(sep);
                    r = sfmt("_sj_%d", tmp);
                }
                else if (strcmp(method, "any?") == 0 && call->block &&
                         PM_NODE_TYPE(call->block) == PM_BLOCK_NODE) {
                    /* arr.any? { |x| cond } → iterate and check */
                    pm_block_node_t *blk = (pm_block_node_t *)call->block;
                    char *bpname = extract_block_param(ctx, blk);
                    int tmp = ctx->temp_counter++;
                    emit(ctx, "mrb_bool _any_%d = FALSE;\n", tmp);
                    emit(ctx, "for (mrb_int _ai_%d = 0; _ai_%d < sp_StrArray_length(%s); _ai_%d++) {\n", tmp, tmp, recv, tmp);
                    ctx->indent++;
                    if (bpname) {
                        char *cn = make_cname(bpname, false);
                        emit(ctx, "const char *%s = (%s)->data[_ai_%d];\n", cn, recv, tmp);
                        free(cn);
                    }
                    if (blk->body) {
                        pm_statements_node_t *stmts = (pm_statements_node_t *)blk->body;
                        if (stmts->body.size > 0) {
                            char *cond = codegen_expr(ctx, stmts->body.nodes[stmts->body.size - 1]);
                            emit(ctx, "if (%s) { _any_%d = TRUE; break; }\n", cond, tmp);
                            free(cond);
                        }
                    }
                    ctx->indent--;
                    emit(ctx, "}\n");
                    free(bpname);
                    r = sfmt("_any_%d", tmp);
                }
                else if (strcmp(method, "find") == 0 && call->block &&
                         PM_NODE_TYPE(call->block) == PM_BLOCK_NODE) {
                    pm_block_node_t *blk = (pm_block_node_t *)call->block;
                    char *bpname = extract_block_param(ctx, blk);
                    int tmp = ctx->temp_counter++;
                    emit(ctx, "const char *_find_%d = \"\";\n", tmp);
                    emit(ctx, "for (mrb_int _fi_%d = 0; _fi_%d < sp_StrArray_length(%s); _fi_%d++) {\n", tmp, tmp, recv, tmp);
                    ctx->indent++;
                    if (bpname) {
                        char *cn = make_cname(bpname, false);
                        emit(ctx, "const char *%s = (%s)->data[_fi_%d];\n", cn, recv, tmp);
                        free(cn);
                    }
                    if (blk->body) {
                        pm_statements_node_t *stmts = (pm_statements_node_t *)blk->body;
                        if (stmts->body.size > 0) {
                            char *cond = codegen_expr(ctx, stmts->body.nodes[stmts->body.size - 1]);
                            emit(ctx, "if (%s) { _find_%d = (%s)->data[_fi_%d]; break; }\n", cond, tmp, recv, tmp);
                            free(cond);
                        }
                    }
                    ctx->indent--;
                    emit(ctx, "}\n");
                    free(bpname);
                    r = sfmt("_find_%d", tmp);
                }
                else if (strcmp(method, "max_by") == 0 && call->block &&
                         PM_NODE_TYPE(call->block) == PM_BLOCK_NODE) {
                    pm_block_node_t *blk = (pm_block_node_t *)call->block;
                    char *bpname = extract_block_param(ctx, blk);
                    int tmp = ctx->temp_counter++;
                    emit(ctx, "const char *_maxby_%d = \"\"; mrb_int _maxval_%d = -9223372036854775807LL;\n", tmp, tmp);
                    emit(ctx, "for (mrb_int _mi_%d = 0; _mi_%d < sp_StrArray_length(%s); _mi_%d++) {\n", tmp, tmp, recv, tmp);
                    ctx->indent++;
                    if (bpname) {
                        char *cn = make_cname(bpname, false);
                        emit(ctx, "const char *%s = (%s)->data[_mi_%d];\n", cn, recv, tmp);
                        free(cn);
                    }
                    if (blk->body) {
                        pm_statements_node_t *stmts = (pm_statements_node_t *)blk->body;
                        if (stmts->body.size > 0) {
                            char *val = codegen_expr(ctx, stmts->body.nodes[stmts->body.size - 1]);
                            emit(ctx, "{ mrb_int _v_%d = %s; if (_v_%d > _maxval_%d) { _maxval_%d = _v_%d; _maxby_%d = (%s)->data[_mi_%d]; } }\n",
                                 tmp, val, tmp, tmp, tmp, tmp, tmp, recv, tmp);
                            free(val);
                        }
                    }
                    ctx->indent--;
                    emit(ctx, "}\n");
                    free(bpname);
                    r = sfmt("_maxby_%d", tmp);
                }
                else if (strcmp(method, "filter_map") == 0 && call->block &&
                         PM_NODE_TYPE(call->block) == PM_BLOCK_NODE) {
                    /* filter_map → just map (no nil in StrArray) */
                    pm_block_node_t *blk = (pm_block_node_t *)call->block;
                    char *bpname = extract_block_param(ctx, blk);
                    int tmp = ctx->temp_counter++;
                    emit(ctx, "sp_StrArray *_fm_%d = sp_StrArray_new();\n", tmp);
                    emit(ctx, "for (mrb_int _fmi_%d = 0; _fmi_%d < sp_StrArray_length(%s); _fmi_%d++) {\n", tmp, tmp, recv, tmp);
                    ctx->indent++;
                    if (bpname) {
                        char *cn = make_cname(bpname, false);
                        emit(ctx, "const char *%s = (%s)->data[_fmi_%d];\n", cn, recv, tmp);
                        free(cn);
                    }
                    if (blk->body) {
                        pm_statements_node_t *stmts = (pm_statements_node_t *)blk->body;
                        if (stmts->body.size > 0) {
                            char *val = codegen_expr(ctx, stmts->body.nodes[stmts->body.size - 1]);
                            emit(ctx, "sp_StrArray_push(_fm_%d, %s);\n", tmp, val);
                            free(val);
                        }
                    }
                    ctx->indent--;
                    emit(ctx, "}\n");
                    free(bpname);
                    r = sfmt("_fm_%d", tmp);
                }
                else if (strcmp(method, "count") == 0 && call->block &&
                         PM_NODE_TYPE(call->block) == PM_BLOCK_NODE) {
                    pm_block_node_t *blk = (pm_block_node_t *)call->block;
                    char *bpname = extract_block_param(ctx, blk);
                    int tmp = ctx->temp_counter++;
                    /* Register block param as STRING for type inference */
                    if (bpname) {
                        var_entry_t *existing = var_lookup(ctx, bpname);
                        if (existing) existing->type = vt_prim(SPINEL_TYPE_STRING);
                        else var_declare(ctx, bpname, vt_prim(SPINEL_TYPE_STRING), false);
                    }
                    emit(ctx, "mrb_int _cnt_%d = 0;\n", tmp);
                    emit(ctx, "for (mrb_int _ci_%d = 0; _ci_%d < sp_StrArray_length(%s); _ci_%d++) {\n", tmp, tmp, recv, tmp);
                    ctx->indent++;
                    if (bpname) {
                        char *cn = make_cname(bpname, false);
                        emit(ctx, "const char *%s = (%s)->data[_ci_%d];\n", cn, recv, tmp);
                        free(cn);
                    }
                    if (blk->body) {
                        pm_statements_node_t *stmts = (pm_statements_node_t *)blk->body;
                        if (stmts->body.size > 0) {
                            char *cond = codegen_expr(ctx, stmts->body.nodes[stmts->body.size - 1]);
                            emit(ctx, "if (%s) _cnt_%d++;\n", cond, tmp);
                            free(cond);
                        }
                    }
                    ctx->indent--;
                    emit(ctx, "}\n");
                    free(bpname);
                    r = sfmt("_cnt_%d", tmp);
                }
                else if (strcmp(method, "count") == 0 && !call->block) {
                    r = sfmt("sp_StrArray_length(%s)", recv);
                }
                if (r) {
                    free(recv); free(method);
                    return r;
                }
                free(recv);
            }

            /* Numeric method calls */
            if (recv_t.kind == SPINEL_TYPE_INTEGER) {
                char *recv = codegen_expr(ctx, call->receiver);
                char *r = NULL;
                if (strcmp(method, "abs") == 0)
                    r = sfmt("((%s) < 0 ? -(%s) : (%s))", recv, recv, recv);
                else if (strcmp(method, "even?") == 0)
                    r = sfmt("((%s) %% 2 == 0)", recv);
                else if (strcmp(method, "odd?") == 0)
                    r = sfmt("((%s) %% 2 != 0)", recv);
                else if (strcmp(method, "zero?") == 0)
                    r = sfmt("((%s) == 0)", recv);
                else if (strcmp(method, "positive?") == 0)
                    r = sfmt("((%s) > 0)", recv);
                else if (strcmp(method, "negative?") == 0)
                    r = sfmt("((%s) < 0)", recv);
                else if (strcmp(method, "**") == 0 && call->arguments &&
                         call->arguments->arguments.size == 1) {
                    char *exp = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
                    r = sfmt("((mrb_int)pow((double)%s, (double)%s))", recv, exp);
                    free(exp);
                }
                else if (strcmp(method, "clamp") == 0 && call->arguments &&
                         call->arguments->arguments.size == 2) {
                    char *lo = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
                    char *hi = codegen_expr(ctx, call->arguments->arguments.nodes[1]);
                    r = sfmt("((%s) < (%s) ? (%s) : (%s) > (%s) ? (%s) : (%s))", recv, lo, lo, recv, hi, hi, recv);
                    free(lo); free(hi);
                }
                else if (strcmp(method, "succ") == 0)
                    r = sfmt("((%s) + 1)", recv);
                else if (strcmp(method, "itself") == 0)
                    r = sfmt("%s", recv);
                if (r) { free(recv); free(method); return r; }
                free(recv);
            }
            if (recv_t.kind == SPINEL_TYPE_FLOAT) {
                char *recv = codegen_expr(ctx, call->receiver);
                char *r = NULL;
                if (strcmp(method, "abs") == 0)
                    r = sfmt("fabs(%s)", recv);
                else if (strcmp(method, "ceil") == 0)
                    r = sfmt("((mrb_int)ceil(%s))", recv);
                else if (strcmp(method, "floor") == 0)
                    r = sfmt("((mrb_int)floor(%s))", recv);
                else if (strcmp(method, "round") == 0)
                    r = sfmt("((mrb_int)round(%s))", recv);
                if (r) { free(recv); free(method); return r; }
                free(recv);
            }

            /* to_sym → no-op (symbols are strings in Spinel) */
            if (strcmp(method, "to_sym") == 0 && call->receiver) {
                char *recv = codegen_expr(ctx, call->receiver);
                free(method);
                return recv;
            }

            /* freeze → no-op (return self expression), frozen? → TRUE */
            if (strcmp(method, "freeze") == 0) {
                char *recv = codegen_expr(ctx, call->receiver);
                free(method);
                return recv; /* freeze returns self, no-op in AOT */
            }
            if (strcmp(method, "frozen?") == 0) {
                codegen_expr(ctx, call->receiver); /* evaluate receiver for side effects */
                free(method);
                return xstrdup("TRUE"); /* everything is effectively frozen in AOT */
            }

            /* Universal methods: nil?, is_a?, respond_to? */
            if (strcmp(method, "nil?") == 0) {
                /* nil? on a POLY value → runtime check */
                if (recv_t.kind == SPINEL_TYPE_POLY) {
                    char *re = codegen_expr(ctx, call->receiver);
                    char *r = sfmt("sp_poly_nil_p(%s)", re);
                    free(re); free(method);
                    return r;
                }
                /* nil? on pointer types → runtime NULL check */
                if (recv_t.kind == SPINEL_TYPE_OBJECT ||
                    recv_t.kind == SPINEL_TYPE_ARRAY ||
                    recv_t.kind == SPINEL_TYPE_FLOAT_ARRAY ||
                    recv_t.kind == SPINEL_TYPE_SP_STRING ||
                    recv_t.kind == SPINEL_TYPE_HASH) {
                    char *re = codegen_expr(ctx, call->receiver);
                    char *r = sfmt("(%s == NULL)", re);
                    free(re); free(method);
                    return r;
                }
                /* nil? is always false for non-nil value types, true for nil */
                if (PM_NODE_TYPE(call->receiver) == PM_NIL_NODE) {
                    free(method); return xstrdup("TRUE");
                }
                free(method); return xstrdup("FALSE");
            }
            if (strcmp(method, "is_a?") == 0 && call->arguments &&
                call->arguments->arguments.size == 1 &&
                PM_NODE_TYPE(call->arguments->arguments.nodes[0]) == PM_CONSTANT_READ_NODE) {
                pm_constant_read_node_t *cr = (pm_constant_read_node_t *)call->arguments->arguments.nodes[0];
                char *cls_name = cstr(ctx, cr->name);
                bool result = false;
                if (recv_t.kind == SPINEL_TYPE_OBJECT) {
                    /* Check class and superclass chain */
                    class_info_t *c = find_class(ctx, recv_t.klass);
                    while (c) {
                        if (strcmp(c->name, cls_name) == 0) { result = true; break; }
                        c = c->superclass[0] ? find_class(ctx, c->superclass) : NULL;
                    }
                } else if (recv_t.kind == SPINEL_TYPE_INTEGER && strcmp(cls_name, "Integer") == 0) result = true;
                else if (recv_t.kind == SPINEL_TYPE_FLOAT && strcmp(cls_name, "Float") == 0) result = true;
                else if (recv_t.kind == SPINEL_TYPE_STRING && strcmp(cls_name, "String") == 0) result = true;
                free(cls_name); free(method);
                return xstrdup(result ? "TRUE" : "FALSE");
            }
            if (strcmp(method, "respond_to?") == 0 && call->arguments &&
                call->arguments->arguments.size == 1 &&
                PM_NODE_TYPE(call->arguments->arguments.nodes[0]) == PM_SYMBOL_NODE) {
                pm_symbol_node_t *sym = (pm_symbol_node_t *)call->arguments->arguments.nodes[0];
                const uint8_t *src = pm_string_source(&sym->unescaped);
                size_t len = pm_string_length(&sym->unescaped);
                char mname[64]; snprintf(mname, sizeof(mname), "%.*s", (int)len, src);
                bool result = false;
                if (recv_t.kind == SPINEL_TYPE_OBJECT) {
                    class_info_t *c = find_class(ctx, recv_t.klass);
                    if (c) {
                        class_info_t *owner;
                        result = find_method_inherited(ctx, c, mname, &owner) != NULL;
                    }
                }
                free(method);
                return xstrdup(result ? "TRUE" : "FALSE");
            }

            if (recv_t.kind == SPINEL_TYPE_OBJECT) {
                class_info_t *cls = find_class(ctx, recv_t.klass);
                if (cls) {
                    class_info_t *owner = NULL;
                    method_info_t *m = find_method_inherited(ctx, cls, method, &owner);
                    if (m) {
                        char *recv = codegen_expr(ctx, call->receiver);

                        /* Inline getter: recv.field (works for inherited fields too) */
                        if (m->is_getter) {
                            char *r;
                            if (cls->is_value_type)
                                r = sfmt("%s.%s", recv, m->accessor_ivar);
                            else
                                r = sfmt("%s->%s", recv, m->accessor_ivar);
                            free(recv); free(method);
                            return r;
                        }

                        /* Inline setter: recv.field = val */
                        if (m->is_setter && call->arguments &&
                            call->arguments->arguments.size == 1) {
                            char *val = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
                            char *r;
                            if (cls->is_value_type)
                                r = sfmt("(%s.%s = %s)", recv, m->accessor_ivar, val);
                            else
                                r = sfmt("(%s->%s = %s)", recv, m->accessor_ivar, val);
                            free(recv); free(val); free(method);
                            return r;
                        }

                        /* Direct method call (with cast if inherited) */
                        int argc = call->arguments ? (int)call->arguments->arguments.size : 0;
                        char *args = xstrdup("");
                        bool has_kw_hash2 = (argc > 0 &&
                            PM_NODE_TYPE(call->arguments->arguments.nodes[argc - 1]) == PM_KEYWORD_HASH_NODE);
                        bool method_has_kw2 = (m->param_count > 0 && m->params[m->param_count - 1].is_keyword);
                        if (has_kw_hash2 && method_has_kw2) {
                            /* Unpack keyword hash for keyword methods */
                            int pos_argc = argc - 1;
                            for (int i = 0; i < pos_argc; i++) {
                                char *a = codegen_expr(ctx, call->arguments->arguments.nodes[i]);
                                char *na = sfmt("%s, %s", args, a);
                                free(args); free(a); args = na;
                            }
                            pm_keyword_hash_node_t *kwh = (pm_keyword_hash_node_t *)call->arguments->arguments.nodes[argc - 1];
                            for (int pi = 0; pi < m->param_count; pi++) {
                                if (!m->params[pi].is_keyword) continue;
                                char *val = NULL;
                                for (size_t ki = 0; ki < kwh->elements.size; ki++) {
                                    if (PM_NODE_TYPE(kwh->elements.nodes[ki]) != PM_ASSOC_NODE) continue;
                                    pm_assoc_node_t *assoc = (pm_assoc_node_t *)kwh->elements.nodes[ki];
                                    if (PM_NODE_TYPE(assoc->key) == PM_SYMBOL_NODE) {
                                        pm_symbol_node_t *ksym = (pm_symbol_node_t *)assoc->key;
                                        const uint8_t *ksrc = pm_string_source(&ksym->unescaped);
                                        size_t klen = pm_string_length(&ksym->unescaped);
                                        char kname[64]; snprintf(kname, sizeof(kname), "%.*s", (int)klen, ksrc);
                                        if (strcmp(kname, m->params[pi].name) == 0) {
                                            val = codegen_expr(ctx, assoc->value);
                                            break;
                                        }
                                    }
                                }
                                if (!val) {
                                    if (m->params[pi].is_optional && m->params[pi].default_node) {
                                        pm_parser_t *sp = ctx->parser;
                                        if (m->origin_parser) ctx->parser = m->origin_parser;
                                        val = codegen_expr(ctx, (pm_node_t *)m->params[pi].default_node);
                                        ctx->parser = sp;
                                    } else {
                                        val = xstrdup("0");
                                    }
                                }
                                char *na = sfmt("%s, %s", args, val);
                                free(args); free(val); args = na;
                            }
                        } else {
                            for (int i = 0; i < argc; i++) {
                                /* Skip keyword hash if method has no keyword params */
                                if (PM_NODE_TYPE(call->arguments->arguments.nodes[i]) == PM_KEYWORD_HASH_NODE
                                    && !method_has_kw2)
                                    continue;
                                char *a = codegen_expr(ctx, call->arguments->arguments.nodes[i]);
                                char *na = sfmt("%s, %s", args, a);
                                free(args); free(a);
                                args = na;
                            }
                        }

                        const char *c_mname = sanitize_method(method);
                        char *r;
                        if (owner && owner != cls) {
                            /* Inherited method: cast receiver to parent type */
                            if (cls->is_value_type)
                                r = sfmt("sp_%s_%s(%s%s)", owner->name, c_mname, recv, args);
                            else
                                r = sfmt("sp_%s_%s((sp_%s *)%s%s)", owner->name, c_mname, owner->name, recv, args);
                        } else {
                            r = sfmt("sp_%s_%s(%s%s)", recv_t.klass, c_mname, recv, args);
                        }
                        free(recv); free(args); free(method);
                        return r;
                    }
                }
            }

            /* Polymorphic dispatch: method call on POLY receiver with known class set */
            if (recv_t.kind == SPINEL_TYPE_POLY && call->receiver &&
                PM_NODE_TYPE(call->receiver) == PM_LOCAL_VARIABLE_READ_NODE) {
                pm_local_variable_read_node_t *lv = (pm_local_variable_read_node_t *)call->receiver;
                char *vname = cstr(ctx, lv->name);
                /* Find which function param this is */
                func_info_t *cur_func = find_func(ctx, ctx->current_func_name);
                if (cur_func) {
                    for (int pi = 0; pi < cur_func->param_count; pi++) {
                        if (strcmp(cur_func->params[pi].name, vname) != 0) continue;
                        char classes[MAX_POLY_CLASSES][64];
                        int nclasses = poly_class_get(ctx, cur_func->name, pi, classes);
                        if (nclasses >= 2) {
                            char *recv = codegen_expr(ctx, call->receiver);
                            const char *c_mname = sanitize_method(method);
                            int argc = call->arguments ? (int)call->arguments->arguments.size : 0;
                            char *args = xstrdup("");
                            for (int i = 0; i < argc; i++) {
                                char *a = codegen_expr(ctx, call->arguments->arguments.nodes[i]);
                                char *na = sfmt("%s, %s", args, a);
                                free(args); free(a);
                                args = na;
                            }
                            /* Determine return type from first class's method */
                            class_info_t *cls0 = find_class(ctx, classes[0]);
                            method_info_t *m0 = cls0 ? find_method_inherited(ctx, cls0, method, NULL) : NULL;
                            bool returns_string = m0 && m0->return_type.kind == SPINEL_TYPE_STRING;

                            /* Megamorphic (3+ types): use dispatch function */
                            if (nclasses >= 3) {
                                spinel_type_t ret_kind = m0 ? m0->return_type.kind : SPINEL_TYPE_NIL;
                                mega_dispatch_register(ctx, method, c_mname, classes, nclasses, ret_kind);
                                char *r;
                                if (returns_string)
                                    r = sfmt("sp_dispatch_%s(%s%s)", c_mname, recv, args);
                                else
                                    r = sfmt("sp_dispatch_%s(%s%s)", c_mname, recv, args);
                                free(recv); free(args); free(vname); free(method);
                                return r;
                            }

                            /* Bimorphic (exactly 2 types): inline if/else */
                            int tmp = ctx->temp_counter++;
                            if (returns_string) {
                                emit(ctx, "const char *_poly_%d;\n", tmp);
                                for (int ci = 0; ci < nclasses; ci++) {
                                    class_info_t *cls = find_class(ctx, classes[ci]);
                                    if (!cls) continue;
                                    const char *cond = ci == 0 ? "if" : "else if";
                                    emit(ctx, "%s (SP_TAG(%s) == SP_TAG_%s) _poly_%d = sp_%s_%s((sp_%s *)sp_unbox_obj(%s)%s);\n",
                                         cond, recv, classes[ci], tmp,
                                         classes[ci], c_mname, classes[ci], recv, args);
                                }
                                emit(ctx, "else _poly_%d = \"\";\n", tmp);
                            } else {
                                /* Generic: box result as sp_RbValue */
                                emit(ctx, "sp_RbValue _poly_%d = sp_box_nil();\n", tmp);
                                for (int ci = 0; ci < nclasses; ci++) {
                                    class_info_t *cls = find_class(ctx, classes[ci]);
                                    if (!cls) continue;
                                    method_info_t *mi = find_method_inherited(ctx, cls, method, NULL);
                                    const char *cond = ci == 0 ? "if" : "else if";
                                    emit(ctx, "%s (SP_TAG(%s) == SP_TAG_%s) {\n", cond, recv, classes[ci]);
                                    char *call_expr = sfmt("sp_%s_%s((sp_%s *)sp_unbox_obj(%s)%s)",
                                                           classes[ci], c_mname, classes[ci], recv, args);
                                    if (mi) {
                                        char *boxed = poly_box_expr_vt(ctx, mi->return_type, call_expr);
                                        emit(ctx, "    _poly_%d = %s;\n", tmp, boxed);
                                        free(boxed);
                                    } else {
                                        emit(ctx, "    _poly_%d = sp_box_nil();\n", tmp);
                                    }
                                    free(call_expr);
                                    emit(ctx, "}\n");
                                }
                            }
                            free(recv); free(args); free(vname); free(method);
                            if (returns_string)
                                return sfmt("_poly_%d", tmp);
                            else
                                return sfmt("_poly_%d", tmp);
                        }
                    }
                }
                free(vname);
            }

            /* to_f, to_i on Integer/Float */
            if (strcmp(method, "to_f") == 0 && recv_t.kind == SPINEL_TYPE_INTEGER) {
                char *recv = codegen_expr(ctx, call->receiver);
                char *r = sfmt("((mrb_float)%s)", recv);
                free(recv); free(method);
                return r;
            }
            if (strcmp(method, "to_s") == 0 && recv_t.kind == SPINEL_TYPE_INTEGER) {
                char *recv = codegen_expr(ctx, call->receiver);
                char *r = sfmt("sp_int_to_s(%s)", recv);
                free(recv); free(method);
                return r;
            }
            if (strcmp(method, "to_s") == 0 && recv_t.kind == SPINEL_TYPE_POLY) {
                char *recv = codegen_expr(ctx, call->receiver);
                char *r = sfmt("sp_poly_to_s(%s)", recv);
                free(recv); free(method);
                return r;
            }
            if (strcmp(method, "to_s") == 0 && recv_t.kind == SPINEL_TYPE_FLOAT) {
                char *recv = codegen_expr(ctx, call->receiver);
                char *r = sfmt("sp_float_to_s(%s)", recv);
                free(recv); free(method);
                return r;
            }
            if (strcmp(method, "to_s") == 0 &&
                (recv_t.kind == SPINEL_TYPE_VALUE || recv_t.kind == SPINEL_TYPE_UNKNOWN)) {
                char *recv = codegen_expr(ctx, call->receiver);
                char *r = sfmt("sp_int_to_s((mrb_int)%s)", recv);
                free(recv); free(method);
                return r;
            }
            if (strcmp(method, "to_i") == 0 && recv_t.kind == SPINEL_TYPE_FLOAT) {
                char *recv = codegen_expr(ctx, call->receiver);
                char *r = sfmt("((mrb_int)%s)", recv);
                free(recv); free(method);
                return r;
            }
            if ((strcmp(method, "to_f") == 0 || strcmp(method, "to_i") == 0) &&
                recv_t.kind != SPINEL_TYPE_STRING) {
                /* Identity for matching types (not strings — handled above) */
                char *recv = codegen_expr(ctx, call->receiver);
                free(method);
                return recv;
            }
        }

        /* Receiver-less: implicit self method call in class body (with inheritance) */
        if (!call->receiver && ctx->current_class) {
            class_info_t *owner = NULL;
            method_info_t *m = find_method_inherited(ctx, ctx->current_class, method, &owner);
            if (m) {
                /* Inline getter: self.field or self->field */
                if (m->is_getter) {
                    char *r;
                    if (ctx->current_class->is_value_type)
                        r = sfmt("self.%s", m->accessor_ivar);
                    else
                        r = sfmt("self->%s", m->accessor_ivar);
                    free(method);
                    return r;
                }
                /* Inline setter */
                if (m->is_setter && call->arguments &&
                    call->arguments->arguments.size == 1) {
                    char *val = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
                    char *r;
                    if (ctx->current_class->is_value_type)
                        r = sfmt("(self.%s = %s)", m->accessor_ivar, val);
                    else
                        r = sfmt("(self->%s = %s)", m->accessor_ivar, val);
                    free(val); free(method);
                    return r;
                }

                int argc = call->arguments ? (int)call->arguments->arguments.size : 0;
                char *args = xstrdup("");
                /* Check if last arg is keyword hash and method has keyword params */
                bool has_kw_hash = (argc > 0 &&
                    PM_NODE_TYPE(call->arguments->arguments.nodes[argc - 1]) == PM_KEYWORD_HASH_NODE);
                bool method_has_kw = (m->param_count > 0 && m->params[m->param_count - 1].is_keyword);
                if (has_kw_hash && method_has_kw) {
                    /* Emit positional args first */
                    int pos_argc = argc - 1;
                    for (int i = 0; i < pos_argc; i++) {
                        char *a = codegen_expr(ctx, call->arguments->arguments.nodes[i]);
                        char *na = sfmt("%s, %s", args, a);
                        free(args); free(a);
                        args = na;
                    }
                    /* Unpack keyword hash by matching against method params */
                    pm_keyword_hash_node_t *kwh = (pm_keyword_hash_node_t *)call->arguments->arguments.nodes[argc - 1];
                    pm_parser_t *saved_p2 = ctx->parser;
                    if (m->origin_parser) ctx->parser = m->origin_parser;
                    for (int pi = 0; pi < m->param_count; pi++) {
                        if (!m->params[pi].is_keyword) continue;
                        char *val = NULL;
                        for (size_t ki = 0; ki < kwh->elements.size; ki++) {
                            if (PM_NODE_TYPE(kwh->elements.nodes[ki]) != PM_ASSOC_NODE) continue;
                            pm_assoc_node_t *assoc = (pm_assoc_node_t *)kwh->elements.nodes[ki];
                            if (PM_NODE_TYPE(assoc->key) == PM_SYMBOL_NODE) {
                                pm_symbol_node_t *ksym = (pm_symbol_node_t *)assoc->key;
                                const uint8_t *ksrc = pm_string_source(&ksym->unescaped);
                                size_t klen = pm_string_length(&ksym->unescaped);
                                char kname[64]; snprintf(kname, sizeof(kname), "%.*s", (int)klen, ksrc);
                                if (strcmp(kname, m->params[pi].name) == 0) {
                                    ctx->parser = saved_p2; /* use caller's parser for value */
                                    val = codegen_expr(ctx, assoc->value);
                                    if (m->origin_parser) ctx->parser = m->origin_parser;
                                    break;
                                }
                            }
                        }
                        if (!val) {
                            /* Use default value if available */
                            if (m->params[pi].is_optional && m->params[pi].default_node) {
                                val = codegen_expr(ctx, (pm_node_t *)m->params[pi].default_node);
                            } else {
                                val = xstrdup("0");
                            }
                        }
                        char *na = sfmt("%s, %s", args, val);
                        free(args); free(val);
                        args = na;
                    }
                    ctx->parser = saved_p2;
                } else {
                    for (int i = 0; i < argc; i++) {
                        /* Skip keyword hash arg if method doesn't have keyword params */
                        if (PM_NODE_TYPE(call->arguments->arguments.nodes[i]) == PM_KEYWORD_HASH_NODE
                            && !method_has_kw)
                            continue;
                        char *a = codegen_expr(ctx, call->arguments->arguments.nodes[i]);
                        char *na = sfmt("%s, %s", args, a);
                        free(args); free(a);
                        args = na;
                    }
                }
                /* Add NULL block params for yield methods called without blocks */
                if (m->body_node && has_yield_nodes(m->body_node) && !call->block) {
                    char *na = sfmt("%s, NULL, NULL", args);
                    free(args);
                    args = na;
                }
                const char *c_mname = sanitize_method(method);
                char *r;
                if (owner && owner != ctx->current_class) {
                    /* Inherited method: cast self to parent type */
                    if (ctx->current_class->is_value_type)
                        r = sfmt("sp_%s_%s(self%s)", owner->name, c_mname, args);
                    else
                        r = sfmt("sp_%s_%s((sp_%s *)self%s)", owner->name, c_mname, owner->name, args);
                } else {
                    r = sfmt("sp_%s_%s(self%s)",
                                   ctx->current_class->name, c_mname, args);
                }
                free(args); free(method);
                return r;
            }
        }

        /* Receiver-less: Integer() conversion — use default value from || */
        if (!call->receiver && strcmp(method, "Integer") == 0) {
            if (call->arguments && call->arguments->arguments.size == 1) {
                pm_node_t *arg = call->arguments->arguments.nodes[0];
                /* Integer(ARGV[0] || 64) → just use the default literal */
                if (PM_NODE_TYPE(arg) == PM_OR_NODE) {
                    pm_or_node_t *or_n = (pm_or_node_t *)arg;
                    char *def = codegen_expr(ctx, or_n->right);
                    free(method);
                    return def;
                }
                char *a = codegen_expr(ctx, arg);
                free(method);
                return a;
            }
            free(method);
            return xstrdup("0");
        }

        /* catch(:tag) { block } → setjmp + block, return value or thrown value */
        if (!call->receiver && strcmp(method, "catch") == 0 &&
            call->arguments && call->arguments->arguments.size >= 1 &&
            call->block && PM_NODE_TYPE(call->block) == PM_BLOCK_NODE) {
            char *tag = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
            pm_block_node_t *blk = (pm_block_node_t *)call->block;
            int tmp = ctx->temp_counter++;
            vtype_t rt = infer_type(ctx, (pm_node_t *)blk->body);
            char *ct = (rt.kind == SPINEL_TYPE_STRING) ? xstrdup("const char *") : xstrdup("mrb_int");
            emit(ctx, "%s _catch_%d; {\n", ct, tmp);
            ctx->indent++;
            emit(ctx, "sp_exc_depth++;\n");
            emit(ctx, "int _cj_%d = setjmp(sp_exc_stack[sp_exc_depth - 1]);\n", tmp);
            emit(ctx, "if (_cj_%d == 0) {\n", tmp);
            ctx->indent++;
            /* Block body — last expression is the return value */
            if (blk->body && PM_NODE_TYPE(blk->body) == PM_STATEMENTS_NODE) {
                pm_statements_node_t *stmts = (pm_statements_node_t *)blk->body;
                for (size_t si = 0; si + 1 < stmts->body.size; si++)
                    codegen_stmt(ctx, stmts->body.nodes[si]);
                if (stmts->body.size > 0) {
                    char *val = codegen_expr(ctx, stmts->body.nodes[stmts->body.size - 1]);
                    emit(ctx, "_catch_%d = %s;\n", tmp, val);
                    free(val);
                }
            }
            emit(ctx, "sp_exc_depth--;\n");
            ctx->indent--;
            emit(ctx, "} else if (_cj_%d == 2 && sp_throw_tag && strcmp(sp_throw_tag, %s) == 0) {\n", tmp, tag);
            ctx->indent++;
            emit(ctx, "sp_exc_depth--;\n");
            if (rt.kind == SPINEL_TYPE_STRING)
                emit(ctx, "_catch_%d = sp_throw_is_str ? sp_throw_value_s : \"\";\n", tmp);
            else
                emit(ctx, "_catch_%d = sp_throw_is_str ? 0 : sp_throw_value_i;\n", tmp);
            emit(ctx, "sp_throw_tag = NULL;\n");
            ctx->indent--;
            emit(ctx, "} else {\n");
            ctx->indent++;
            emit(ctx, "sp_exc_depth--;\n");
            emit(ctx, "if (sp_exc_depth > 0) longjmp(sp_exc_stack[sp_exc_depth - 1], _cj_%d);\n", tmp);
            ctx->indent--;
            emit(ctx, "}\n");
            ctx->indent--;
            emit(ctx, "}\n");
            free(tag); free(ct); free(method);
            return sfmt("_catch_%d", tmp);
        }

        /* throw in expression context → emit as statement, return 0 */
        if (!call->receiver && strcmp(method, "throw") == 0 &&
            call->arguments && call->arguments->arguments.size >= 1) {
            codegen_stmt(ctx, node); /* emit the throw */
            free(method);
            return xstrdup("0 /* throw */");
        }

        /* block_given? → check if _block is non-NULL */
        if (!call->receiver && strcmp(method, "block_given?") == 0) {
            free(method);
            return xstrdup("(_block != NULL)");
        }

        /* __method__ → current method/function name as symbol string */
        if (!call->receiver && strcmp(method, "__method__") == 0) {
            free(method);
            if (ctx->current_method)
                return sfmt("\"%s\"", ctx->current_method->name);
            if (ctx->current_func_name[0])
                return sfmt("\"%s\"", ctx->current_func_name);
            return xstrdup("\"main\"");
        }

        /* __dir__ → directory of the source file */
        if (!call->receiver && strcmp(method, "__dir__") == 0) {
            free(method);
            const char *path = ctx->source_path ? ctx->source_path : ".";
            /* Extract directory part at compile time */
            char dir[512];
            strncpy(dir, path, sizeof(dir) - 1);
            dir[sizeof(dir) - 1] = '\0';
            char *slash = strrchr(dir, '/');
            if (slash) *slash = '\0';
            else strcpy(dir, ".");
            return sfmt("\"%s\"", dir);
        }

        /* method(:name) → sp_Proc_new(sp_<name>, NULL) */
        if (!call->receiver && strcmp(method, "method") == 0 &&
            call->arguments && call->arguments->arguments.size == 1 &&
            PM_NODE_TYPE(call->arguments->arguments.nodes[0]) == PM_SYMBOL_NODE) {
            pm_symbol_node_t *sym = (pm_symbol_node_t *)call->arguments->arguments.nodes[0];
            const uint8_t *src = pm_string_source(&sym->unescaped);
            size_t len = pm_string_length(&sym->unescaped);
            char fname[64];
            size_t copy_len = len < 63 ? len : 63;
            memcpy(fname, src, copy_len);
            fname[copy_len] = '\0';
            ctx->needs_proc = true;
            free(method);
            { char *sf = c_safe_name(fname); char *r = sfmt("sp_Proc_new((sp_block_fn)sp_%s, NULL)", sf); free(sf); return r; }
        }

        /* rand(n) → rand() % n */
        if (!call->receiver && strcmp(method, "rand") == 0) {
            if (call->arguments && call->arguments->arguments.size == 1) {
                char *arg = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
                char *r = sfmt("((mrb_int)(rand() %% (int)%s))", arg);
                free(arg); free(method);
                return r;
            }
            free(method);
            return xstrdup("((mrb_int)rand())");
        }

        /* Receiver-less: top-level function or Kernel method */
        if (!call->receiver) {
            func_info_t *fn = find_func(ctx, method);
            if (fn) {
                int argc = call->arguments ? (int)call->arguments->arguments.size : 0;

                /* Check if any argument is a KeywordHashNode (keyword args at call site) */
                bool has_kwarg_call = false;
                pm_keyword_hash_node_t *kw_hash = NULL;
                int kw_hash_idx = -1;
                for (int i = 0; i < argc; i++) {
                    if (PM_NODE_TYPE(call->arguments->arguments.nodes[i]) == PM_KEYWORD_HASH_NODE) {
                        kw_hash = (pm_keyword_hash_node_t *)call->arguments->arguments.nodes[i];
                        kw_hash_idx = i;
                        has_kwarg_call = true;
                        break;
                    }
                }

                if (has_kwarg_call && kw_hash) {
                    /* Keyword argument call: build args array indexed by param position */
                    char *param_args[MAX_PARAMS];
                    for (int i = 0; i < fn->param_count; i++) param_args[i] = NULL;

                    /* First, fill positional (non-keyword) args */
                    int pos_idx = 0;
                    for (int i = 0; i < argc; i++) {
                        if (i == kw_hash_idx) continue;
                        if (pos_idx < fn->param_count && !fn->params[pos_idx].is_keyword)
                            param_args[pos_idx] = codegen_expr(ctx, call->arguments->arguments.nodes[i]);
                        pos_idx++;
                    }

                    /* Then, match keyword args by name to param positions */
                    for (size_t ki = 0; ki < kw_hash->elements.size; ki++) {
                        pm_node_t *elem = kw_hash->elements.nodes[ki];
                        if (PM_NODE_TYPE(elem) != PM_ASSOC_NODE) continue;
                        pm_assoc_node_t *assoc = (pm_assoc_node_t *)elem;
                        /* Get key name from SymbolNode */
                        if (PM_NODE_TYPE(assoc->key) != PM_SYMBOL_NODE) continue;
                        pm_symbol_node_t *sym = (pm_symbol_node_t *)assoc->key;
                        const uint8_t *ksrc = pm_string_source(&sym->unescaped);
                        size_t klen = pm_string_length(&sym->unescaped);
                        char kname[64];
                        size_t copy_len = klen < 63 ? klen : 63;
                        memcpy(kname, ksrc, copy_len);
                        kname[copy_len] = '\0';

                        /* Find matching param */
                        for (int pi = 0; pi < fn->param_count; pi++) {
                            if (strcmp(fn->params[pi].name, kname) == 0) {
                                param_args[pi] = codegen_expr(ctx, assoc->value);
                                break;
                            }
                        }
                    }

                    /* Fill defaults for missing optional keyword params */
                    for (int i = 0; i < fn->param_count; i++) {
                        if (!param_args[i] && fn->params[i].is_optional && fn->params[i].default_node)
                            param_args[i] = codegen_expr(ctx, (pm_node_t *)fn->params[i].default_node);
                    }

                    /* Build argument string in parameter order */
                    char *args = xstrdup("");
                    for (int i = 0; i < fn->param_count; i++) {
                        if (param_args[i]) {
                            char *na = sfmt("%s%s%s", args, i > 0 ? ", " : "", param_args[i]);
                            free(args);
                            args = na;
                            free(param_args[i]);
                        }
                    }
                    char *r = sfmt("sp_%s(%s)", fn->name, args);
                    free(args); free(method);
                    return r;
                }

                if (fn->has_rest) {
                    /* Rest/splat parameter call: collect positional args into IntArray */
                    int rest_idx = fn->rest_param_index;
                    char *args = xstrdup("");
                    /* Emit positional args before the rest param */
                    int ai = 0;
                    for (int i = 0; i < rest_idx && ai < argc; i++, ai++) {
                        char *a = codegen_expr(ctx, call->arguments->arguments.nodes[ai]);
                        char *na = sfmt("%s%s%s", args, i > 0 ? ", " : "", a);
                        free(args); free(a);
                        args = na;
                    }
                    /* Build the rest array from remaining args */
                    int tmp = ctx->temp_counter++;
                    emit(ctx, "sp_IntArray *_rest_%d = sp_IntArray_new();\n", tmp);
                    for (int i = ai; i < argc; i++) {
                        char *a = codegen_expr(ctx, call->arguments->arguments.nodes[i]);
                        emit(ctx, "sp_IntArray_push(_rest_%d, %s);\n", tmp, a);
                        free(a);
                    }
                    /* Add rest array to args */
                    char *rest_ref = sfmt("_rest_%d", tmp);
                    char *na = sfmt("%s%s%s", args, (rest_idx > 0) ? ", " : "", rest_ref);
                    free(args); free(rest_ref);
                    args = na;

                    /* If target function uses yield, pass block (or NULL if no block) */
                    if (fn->has_yield) {
                        if (call->block && PM_NODE_TYPE(call->block) == PM_BLOCK_NODE) {
                            char *na2 = sfmt("%s, _block, _block_env", args);
                            free(args); args = na2;
                        } else {
                            char *na2 = sfmt("%s, NULL, NULL", args);
                            free(args); args = na2;
                        }
                    }
                    char *r = sfmt("sp_%s(%s)", fn->name, args);
                    free(args); free(method);
                    return r;
                }

                bool _yield_has_nonlocal = false;
                int _yield_blk_id = -1;
                char *args = xstrdup("");
                for (int i = 0; i < argc; i++) {
                    char *a = codegen_expr(ctx, call->arguments->arguments.nodes[i]);
                    /* Box argument if target param is POLY but arg is mono */
                    if (i < fn->param_count && fn->params[i].type.kind == SPINEL_TYPE_POLY) {
                        vtype_t at = infer_type(ctx, call->arguments->arguments.nodes[i]);
                        if (at.kind != SPINEL_TYPE_POLY) {
                            char *boxed = poly_box_expr_vt(ctx, at, a);
                            free(a);
                            a = boxed;
                        }
                    }
                    char *na = sfmt("%s%s%s", args, i > 0 ? ", " : "", a);
                    free(args); free(a);
                    args = na;
                }
                /* Fill in default values for optional parameters */
                for (int i = argc; i < fn->param_count; i++) {
                    if (fn->params[i].is_optional && fn->params[i].default_node) {
                        char *def = codegen_expr(ctx, (pm_node_t *)fn->params[i].default_node);
                        char *na = sfmt("%s%s%s", args, i > 0 ? ", " : "", def);
                        free(args); free(def);
                        args = na;
                    }
                }
                /* If target function uses yield, pass block (or NULL) */
                if (fn->has_yield) {
                    int total = fn->param_count;
                    if (call->block && PM_NODE_TYPE(call->block) == PM_BLOCK_NODE && ctx->in_yield_func) {
                        /* Forward the block from the enclosing yield function */
                        char *na = sfmt("%s%s_block, _block_env", args, total > 0 ? ", " : "");
                        free(args); args = na;
                    } else if (call->block && PM_NODE_TYPE(call->block) == PM_BLOCK_NODE) {
                        /* Generate block callback for yield function call in expression context */
                        pm_block_node_t *blk = (pm_block_node_t *)call->block;
                        int blk_id = ctx->block_counter++;
                        bool has_nonlocal_return = blk->body && block_has_return((pm_node_t *)blk->body);
                        _yield_has_nonlocal = has_nonlocal_return;
                        _yield_blk_id = blk_id;

                        char *bpname = extract_block_param(ctx, blk);

                        capture_list_t local_defs = {.count = 0};
                        capture_list_t captures = {.count = 0};
                        scan_captures(ctx, (pm_node_t *)blk->body,
                                      bpname ? bpname : "", &local_defs, &captures);

                        int saved_indent2 = ctx->indent;
                        int saved_var_count2 = ctx->var_count;
                        ctx->indent = 1;
                        if (bpname)
                            var_declare(ctx, bpname, vt_prim(SPINEL_TYPE_INTEGER), false);

                        char *body_processed = NULL;
                        {
                            char *body_buf_data = NULL;
                            size_t body_buf_size = 0;
                            FILE *body_buf = open_memstream(&body_buf_data, &body_buf_size);
                            FILE *saved_out2 = ctx->out;
                            ctx->out = body_buf;

                            bool saved_in_block_nonlocal = ctx->in_block_nonlocal;
                            int saved_block_return_id = ctx->block_return_id;
                            if (has_nonlocal_return) {
                                ctx->in_block_nonlocal = true;
                                ctx->block_return_id = blk_id;
                            }

                            if (blk->body) codegen_stmts(ctx, (pm_node_t *)blk->body);

                            ctx->in_block_nonlocal = saved_in_block_nonlocal;
                            ctx->block_return_id = saved_block_return_id;

                            fclose(body_buf);
                            ctx->out = saved_out2;

                            if (body_buf_data) {
                                body_processed = xstrdup(body_buf_data);
                                for (int ci = 0; ci < captures.count; ci++) {
                                    char *old_ref = sfmt("lv_%s", captures.names[ci]);
                                    char *new_ref = sfmt("(*_e->%s)", captures.names[ci]);
                                    while (1) {
                                        char *pos = strstr(body_processed, old_ref);
                                        if (!pos) break;
                                        size_t prefix_len = pos - body_processed;
                                        size_t old_len = strlen(old_ref);
                                        size_t new_len = strlen(new_ref);
                                        size_t rest_len = strlen(pos + old_len);
                                        char *nr = malloc(prefix_len + new_len + rest_len + 1);
                                        memcpy(nr, body_processed, prefix_len);
                                        memcpy(nr + prefix_len, new_ref, new_len);
                                        memcpy(nr + prefix_len + new_len, pos + old_len, rest_len + 1);
                                        free(body_processed);
                                        body_processed = nr;
                                    }
                                    free(old_ref); free(new_ref);
                                }
                                free(body_buf_data);
                            }
                        }
                        ctx->indent = saved_indent2;
                        ctx->var_count = saved_var_count2;

                        if (ctx->block_out) {
                            fprintf(ctx->block_out, "typedef struct { ");
                            for (int ci = 0; ci < captures.count; ci++)
                                fprintf(ctx->block_out, "mrb_int *%s; ", captures.names[ci]);
                            if (has_nonlocal_return) {
                                fprintf(ctx->block_out, "volatile mrb_int *_blk_ret_val; int _blk_ret_depth; ");
                            }
                            if (captures.count == 0 && !has_nonlocal_return) fprintf(ctx->block_out, "int _dummy; ");
                            fprintf(ctx->block_out, "} _blk_%d_env;\n", blk_id);
                            fprintf(ctx->block_out, "static mrb_int _blk_%d(void *_env, mrb_int _arg) {\n", blk_id);
                            fprintf(ctx->block_out, "    _blk_%d_env *_e = (_blk_%d_env *)_env;\n", blk_id, blk_id);
                            if (bpname) {
                                char *cn = make_cname(bpname, false);
                                fprintf(ctx->block_out, "    mrb_int %s = _arg;\n", cn);
                                free(cn);
                            }
                            if (body_processed) fprintf(ctx->block_out, "%s", body_processed);
                            fprintf(ctx->block_out, "    return 0;\n");
                            fprintf(ctx->block_out, "}\n\n");
                        }
                        free(body_processed);

                        if (has_nonlocal_return) {
                            emit(ctx, "volatile mrb_int _blk_ret_val_%d = 0;\n", blk_id);
                            emit(ctx, "sp_exc_depth++;\n");
                            emit(ctx, "_blk_%d_env _env_%d = { ", blk_id, blk_id);
                            for (int ci = 0; ci < captures.count; ci++) {
                                char *cn = make_cname(captures.names[ci], false);
                                emit_raw(ctx, "&%s, ", cn);
                                free(cn);
                            }
                            emit_raw(ctx, "&_blk_ret_val_%d, sp_exc_depth - 1 };\n", blk_id);
                        } else {
                            emit(ctx, "_blk_%d_env _env_%d = { ", blk_id, blk_id);
                            for (int ci = 0; ci < captures.count; ci++) {
                                char *cn = make_cname(captures.names[ci], false);
                                emit_raw(ctx, "%s&%s", ci > 0 ? ", " : "", cn);
                                free(cn);
                            }
                            if (captures.count == 0) emit_raw(ctx, "0");
                            emit_raw(ctx, " };\n");
                        }

                        char *na = sfmt("%s%s(sp_block_fn)_blk_%d, &_env_%d", args, total > 0 ? ", " : "", blk_id, blk_id);
                        free(args); args = na;
                        free(bpname);
                    } else {
                        /* No block or not in yield context → pass NULL */
                        char *na = sfmt("%s%sNULL, NULL", args, total > 0 ? ", " : "");
                        free(args); args = na;
                    }
                }
                /* If target function has &block param, wrap block in sp_Proc and pass it */
                if (fn->has_block_param && !ctx->lambda_mode) {
                    int total = fn->param_count;
                    if (call->block && PM_NODE_TYPE(call->block) == PM_BLOCK_NODE) {
                        pm_block_node_t *blk = (pm_block_node_t *)call->block;
                        int blk_id = ctx->block_counter++;
                        ctx->needs_proc = true;

                        char *bpname = NULL;
                        if (blk->parameters && PM_NODE_TYPE(blk->parameters) == PM_BLOCK_PARAMETERS_NODE) {
                            pm_block_parameters_node_t *bp = (pm_block_parameters_node_t *)blk->parameters;
                            if (bp->parameters && bp->parameters->requireds.size > 0) {
                                pm_node_t *p = bp->parameters->requireds.nodes[0];
                                if (PM_NODE_TYPE(p) == PM_REQUIRED_PARAMETER_NODE)
                                    bpname = cstr(ctx, ((pm_required_parameter_node_t *)p)->name);
                            }
                        }

                        capture_list_t local_defs = {.count = 0};
                        capture_list_t captures = {.count = 0};
                        scan_captures(ctx, (pm_node_t *)blk->body,
                                      bpname ? bpname : "", &local_defs, &captures);

                        int saved_indent2 = ctx->indent;
                        int saved_var_count2 = ctx->var_count;
                        ctx->indent = 1;
                        if (bpname)
                            var_declare(ctx, bpname, vt_prim(SPINEL_TYPE_INTEGER), false);

                        char *body_processed = NULL;
                        {
                            char *body_buf_data = NULL;
                            size_t body_buf_size = 0;
                            FILE *body_buf = open_memstream(&body_buf_data, &body_buf_size);
                            FILE *saved_out2 = ctx->out;
                            ctx->out = body_buf;
                            if (blk->body) {
                                /* Generate block body; last expression becomes the return value */
                                if (PM_NODE_TYPE((pm_node_t *)blk->body) == PM_STATEMENTS_NODE) {
                                    pm_statements_node_t *bstmts = (pm_statements_node_t *)blk->body;
                                    for (size_t bi = 0; bi + 1 < bstmts->body.size; bi++)
                                        codegen_stmt(ctx, bstmts->body.nodes[bi]);
                                    if (bstmts->body.size > 0) {
                                        pm_node_t *last_s = bstmts->body.nodes[bstmts->body.size - 1];
                                        vtype_t last_t = infer_type(ctx, last_s);
                                        if (last_t.kind == SPINEL_TYPE_NIL || last_t.kind == SPINEL_TYPE_UNKNOWN) {
                                            codegen_stmt(ctx, last_s);
                                        } else {
                                            char *rv = codegen_expr(ctx, last_s);
                                            emit(ctx, "return %s;\n", rv);
                                            free(rv);
                                        }
                                    }
                                } else {
                                    codegen_stmts(ctx, (pm_node_t *)blk->body);
                                }
                            }
                            fclose(body_buf);
                            ctx->out = saved_out2;
                            if (body_buf_data) {
                                body_processed = xstrdup(body_buf_data);
                                for (int ci = 0; ci < captures.count; ci++) {
                                    char *old_ref = sfmt("lv_%s", captures.names[ci]);
                                    char *new_ref = sfmt("(*_e->%s)", captures.names[ci]);
                                    while (1) {
                                        char *pos = strstr(body_processed, old_ref);
                                        if (!pos) break;
                                        size_t prefix_len = pos - body_processed;
                                        size_t old_len = strlen(old_ref);
                                        size_t new_len = strlen(new_ref);
                                        size_t rest_len = strlen(pos + old_len);
                                        char *nr = malloc(prefix_len + new_len + rest_len + 1);
                                        memcpy(nr, body_processed, prefix_len);
                                        memcpy(nr + prefix_len, new_ref, new_len);
                                        memcpy(nr + prefix_len + new_len, pos + old_len, rest_len + 1);
                                        free(body_processed);
                                        body_processed = nr;
                                    }
                                    free(old_ref); free(new_ref);
                                }
                                free(body_buf_data);
                            }
                        }
                        ctx->indent = saved_indent2;
                        ctx->var_count = saved_var_count2;

                        if (ctx->block_out) {
                            fprintf(ctx->block_out, "typedef struct { ");
                            for (int ci = 0; ci < captures.count; ci++)
                                fprintf(ctx->block_out, "mrb_int *%s; ", captures.names[ci]);
                            if (captures.count == 0) fprintf(ctx->block_out, "int _dummy; ");
                            fprintf(ctx->block_out, "} _blk_%d_env;\n", blk_id);
                            fprintf(ctx->block_out, "static mrb_int _blk_%d(void *_env, mrb_int _arg) {\n", blk_id);
                            fprintf(ctx->block_out, "    _blk_%d_env *_e = (_blk_%d_env *)_env;\n", blk_id, blk_id);
                            if (bpname) {
                                char *cn = make_cname(bpname, false);
                                fprintf(ctx->block_out, "    mrb_int %s = _arg;\n", cn);
                                free(cn);
                            }
                            if (body_processed) fprintf(ctx->block_out, "%s", body_processed);
                            fprintf(ctx->block_out, "    return 0;\n");
                            fprintf(ctx->block_out, "}\n\n");
                        }
                        free(body_processed);

                        /* Create sp_Proc on the stack and pass pointer */
                        int ptmp = ctx->temp_counter++;
                        if (captures.count > 0) {
                            emit(ctx, "_blk_%d_env _env_%d = { ", blk_id, blk_id);
                            for (int ci = 0; ci < captures.count; ci++) {
                                char *cn = make_cname(captures.names[ci], false);
                                emit_raw(ctx, "%s&%s", ci > 0 ? ", " : "", cn);
                                free(cn);
                            }
                            emit_raw(ctx, " };\n");
                            emit(ctx, "sp_Proc _bp_%d = { (sp_block_fn)_blk_%d, &_env_%d };\n",
                                 ptmp, blk_id, blk_id);
                        } else {
                            emit(ctx, "sp_Proc _bp_%d = { (sp_block_fn)_blk_%d, NULL };\n",
                                 ptmp, blk_id);
                        }
                        char *na = sfmt("%s%s&_bp_%d", args, total > 0 ? ", " : "", ptmp);
                        free(args); args = na;
                        free(bpname);
                    } else {
                        /* No block provided → pass NULL */
                        char *na = sfmt("%s%sNULL", args, total > 0 ? ", " : "");
                        free(args); args = na;
                    }
                }
                if (_yield_has_nonlocal) {
                    /* Non-local return: wrap call with setjmp, use temp for result.
                     * sp_exc_depth was already incremented before env init. */
                    int tmp = ctx->temp_counter++;
                    emit(ctx, "volatile mrb_int _nlr_%d = 0;\n", tmp);
                    emit(ctx, "if (setjmp(sp_exc_stack[sp_exc_depth - 1]) == 0) {\n");
                    ctx->indent++;
                    emit(ctx, "_nlr_%d = sp_%s(%s);\n", tmp, fn->name, args);
                    emit(ctx, "sp_exc_depth--;\n");
                    ctx->indent--;
                    emit(ctx, "} else {\n");
                    ctx->indent++;
                    emit(ctx, "sp_exc_depth--;\n");
                    emit(ctx, "_nlr_%d = _blk_ret_val_%d;\n", tmp, _yield_blk_id);
                    ctx->indent--;
                    emit(ctx, "}\n");
                    free(args); free(method);
                    return sfmt("_nlr_%d", tmp);
                }
                char *r = sfmt("sp_%s(%s)", fn->name, args);
                free(args); free(method);
                return r;
            }
        }

        /* In lambda mode: handle map with block on ValArray result */
        if (ctx->lambda_mode && strcmp(method, "map") == 0 &&
            call->receiver && call->block &&
            PM_NODE_TYPE(call->block) == PM_BLOCK_NODE) {
            /* Check if receiver is a to_array call or variable of ValArray type */
            char *recv = codegen_expr(ctx, call->receiver);
            pm_block_node_t *blk = (pm_block_node_t *)call->block;

            /* Get block parameter name */
            char *bpname = NULL;
            if (blk->parameters && PM_NODE_TYPE(blk->parameters) == PM_BLOCK_PARAMETERS_NODE) {
                pm_block_parameters_node_t *bp = (pm_block_parameters_node_t *)blk->parameters;
                if (bp->parameters && bp->parameters->requireds.size > 0) {
                    pm_node_t *p = bp->parameters->requireds.nodes[0];
                    if (PM_NODE_TYPE(p) == PM_REQUIRED_PARAMETER_NODE)
                        bpname = cstr(ctx, ((pm_required_parameter_node_t *)p)->name);
                }
            }

            /* Generate: { sp_ValArray *_arr = recv; sp_StrArray *_res = ...; for ... } */
            int tmp = ctx->temp_counter++;
            emit(ctx, "sp_ValArray *_va_%d = %s;\n", tmp, recv);
            emit(ctx, "sp_StrArray *_sa_%d = sp_StrArray_new();\n", tmp);
            emit(ctx, "for (int _mi_%d = 0; _mi_%d < _va_%d->len; _mi_%d++) {\n",
                 tmp, tmp, tmp, tmp);
            ctx->indent++;
            if (bpname) {
                emit(ctx, "sp_Val *lv_%s = _va_%d->data[_mi_%d];\n", bpname, tmp, tmp);
            }
            /* Generate the block body as an expression */
            char *body_expr = NULL;
            if (blk->body) {
                pm_node_t *body = (pm_node_t *)blk->body;
                if (PM_NODE_TYPE(body) == PM_STATEMENTS_NODE) {
                    pm_statements_node_t *stmts = (pm_statements_node_t *)body;
                    if (stmts->body.size > 0)
                        body_expr = codegen_expr(ctx, stmts->body.nodes[stmts->body.size - 1]);
                } else {
                    body_expr = codegen_expr(ctx, body);
                }
            }
            if (body_expr) {
                emit(ctx, "sp_StrArray_push(_sa_%d, %s);\n", tmp, body_expr);
                free(body_expr);
            }
            ctx->indent--;
            emit(ctx, "}\n");

            free(recv); free(bpname); free(method);
            return sfmt("_sa_%d", tmp);
        }

        /* In lambda mode: handle join on StrArray */
        if (ctx->lambda_mode && strcmp(method, "join") == 0 && call->receiver) {
            char *recv = codegen_expr(ctx, call->receiver);
            /* This should be an sp_StrArray; join all strings */
            int tmp = ctx->temp_counter++;
            emit(ctx, "size_t _jl_%d = 0;\n", tmp);
            emit(ctx, "for (int _ji_%d = 0; _ji_%d < %s->len; _ji_%d++) _jl_%d += strlen(%s->data[_ji_%d]);\n",
                 tmp, tmp, recv, tmp, tmp, recv, tmp);
            emit(ctx, "char *_js_%d = (char *)malloc(_jl_%d + 1); _js_%d[0] = '\\0';\n", tmp, tmp, tmp);
            emit(ctx, "for (int _ji_%d = 0; _ji_%d < %s->len; _ji_%d++) strcat(_js_%d, %s->data[_ji_%d]);\n",
                 tmp, tmp, recv, tmp, tmp, recv, tmp);
            free(recv); free(method);
            return sfmt("_js_%d", tmp);
        }

        /* In lambda mode: handle slice on string literals */
        if (ctx->lambda_mode && strcmp(method, "slice") == 0 &&
            call->receiver && call->arguments &&
            call->arguments->arguments.size == 1) {
            char *recv = codegen_expr(ctx, call->receiver);
            char *idx = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
            int tmp = ctx->temp_counter++;
            emit(ctx, "char *_sl_%d = (char *)malloc(2); _sl_%d[0] = %s[(int)%s]; _sl_%d[1] = '\\0';\n",
                 tmp, tmp, recv, idx, tmp);
            free(recv); free(idx); free(method);
            return sfmt("_sl_%d", tmp);
        }

        /* report_duration(:label) { block } → just execute the block, return block result */
        if (!call->receiver && strcmp(method, "report_duration") == 0 &&
            call->block && PM_NODE_TYPE(call->block) == PM_BLOCK_NODE) {
            pm_block_node_t *blk = (pm_block_node_t *)call->block;
            if (blk->body) {
                free(method);
                return codegen_expr(ctx, blk->body);
            }
            free(method);
            return xstrdup("0");
        }

        /* Fallback — unsupported method call */
        {
            /* Get receiver type description for warning */
            const char *recv_desc = "unknown";
            if (call->receiver) {
                vtype_t rt = infer_type(ctx, call->receiver);
                if (rt.kind == SPINEL_TYPE_INTEGER) recv_desc = "Integer";
                else if (rt.kind == SPINEL_TYPE_FLOAT) recv_desc = "Float";
                else if (rt.kind == SPINEL_TYPE_STRING) recv_desc = "String";
                else if (rt.kind == SPINEL_TYPE_ARRAY) recv_desc = "Array";
                else if (rt.kind == SPINEL_TYPE_HASH) recv_desc = "Hash";
                else if (rt.kind == SPINEL_TYPE_OBJECT) recv_desc = rt.klass;
                else if (rt.kind == SPINEL_TYPE_POLY) recv_desc = "POLY";
            } else {
                recv_desc = "Kernel";
            }
            fprintf(stderr, "spinel: warning: unsupported call %s#%s\n", recv_desc, method);
            char *r = sfmt("0 /* unsupported: %s#%s */", recv_desc, method);
            free(method);
            return r;
        }
    }

    case PM_IF_NODE: {
        /* Ternary in expression context */
        pm_if_node_t *n = (pm_if_node_t *)node;
        char *cond = codegen_expr(ctx, n->predicate);
        char *then_e = xstrdup("0");
        char *else_e = xstrdup("0");
        if (n->statements && PM_NODE_TYPE((pm_node_t *)n->statements) == PM_STATEMENTS_NODE) {
            pm_statements_node_t *s = (pm_statements_node_t *)n->statements;
            if (s->body.size > 0) { free(then_e); then_e = codegen_expr(ctx, s->body.nodes[0]); }
        }
        if (n->subsequent) {
            if (PM_NODE_TYPE(n->subsequent) == PM_ELSE_NODE) {
                pm_else_node_t *el = (pm_else_node_t *)n->subsequent;
                if (el->statements && el->statements->body.size > 0) {
                    free(else_e);
                    else_e = codegen_expr(ctx, el->statements->body.nodes[0]);
                }
            }
        }
        char *r = sfmt("(%s ? %s : %s)", cond, then_e, else_e);
        free(cond); free(then_e); free(else_e);
        return r;
    }

    case PM_UNLESS_NODE: {
        pm_unless_node_t *n = (pm_unless_node_t *)node;
        vtype_t rt = infer_type(ctx, node);
        char *ct = vt_ctype(ctx, rt, false);
        int tmp = ctx->temp_counter++;
        char *cond = codegen_expr(ctx, n->predicate);
        emit(ctx, "%s _unl_%d;\n", ct, tmp);
        emit(ctx, "if (!(%s)) {\n", cond);
        free(cond);
        ctx->indent++;
        if (n->statements) {
            pm_statements_node_t *stmts = (pm_statements_node_t *)n->statements;
            if (stmts->body.size > 0) {
                char *val = codegen_expr(ctx, stmts->body.nodes[stmts->body.size - 1]);
                emit(ctx, "_unl_%d = %s;\n", tmp, val);
                free(val);
            }
        }
        ctx->indent--;
        if (n->else_clause) {
            emit(ctx, "} else {\n");
            ctx->indent++;
            pm_else_node_t *el = (pm_else_node_t *)n->else_clause;
            if (el->statements) {
                pm_statements_node_t *stmts = (pm_statements_node_t *)el->statements;
                if (stmts->body.size > 0) {
                    char *val = codegen_expr(ctx, stmts->body.nodes[stmts->body.size - 1]);
                    emit(ctx, "_unl_%d = %s;\n", tmp, val);
                    free(val);
                }
            }
            ctx->indent--;
        }
        emit(ctx, "}\n");
        free(ct);
        return sfmt("_unl_%d", tmp);
    }

    case PM_PARENTHESES_NODE: {
        pm_parentheses_node_t *n = (pm_parentheses_node_t *)node;
        if (n->body) {
            if (PM_NODE_TYPE(n->body) == PM_STATEMENTS_NODE) {
                pm_statements_node_t *s = (pm_statements_node_t *)n->body;
                if (s->body.size > 0)
                    return codegen_expr(ctx, s->body.nodes[s->body.size - 1]);
            }
            return codegen_expr(ctx, n->body);
        }
        return xstrdup("0");
    }

    case PM_STATEMENTS_NODE: {
        pm_statements_node_t *s = (pm_statements_node_t *)node;
        if (s->body.size > 0) return codegen_expr(ctx, s->body.nodes[s->body.size - 1]);
        return xstrdup("0");
    }

    case PM_AND_NODE: {
        pm_and_node_t *n = (pm_and_node_t *)node;
        char *left = codegen_expr(ctx, n->left);
        char *right = codegen_expr(ctx, n->right);
        char *r = sfmt("(%s && %s)", left, right);
        free(left); free(right);
        return r;
    }

    case PM_OR_NODE: {
        pm_or_node_t *n = (pm_or_node_t *)node;
        char *left = codegen_expr(ctx, n->left);
        char *right = codegen_expr(ctx, n->right);
        char *r = sfmt("(%s || %s)", left, right);
        free(left); free(right);
        return r;
    }

    case PM_CONSTANT_PATH_NODE: {
        /* Module::method — handled at call site */
        pm_constant_path_node_t *n = (pm_constant_path_node_t *)node;
        if (n->parent && PM_NODE_TYPE(n->parent) == PM_CONSTANT_READ_NODE) {
            pm_constant_read_node_t *p = (pm_constant_read_node_t *)n->parent;
            char *mod_name = cstr(ctx, p->name);
            char *child_name = cstr(ctx, n->name);
            /* Float::INFINITY and Float::NAN */
            if (strcmp(mod_name, "Float") == 0) {
                if (strcmp(child_name, "INFINITY") == 0) {
                    free(mod_name); free(child_name);
                    return xstrdup("(1.0/0.0)");
                }
                if (strcmp(child_name, "NAN") == 0) {
                    free(mod_name); free(child_name);
                    return xstrdup("(0.0/0.0)");
                }
            }
            /* For constants like BNUM, BNUMF */
            char *r = sfmt("sp_%s_%s", mod_name, child_name);
            free(mod_name); free(child_name);
            return r;
        }
        return xstrdup("0");
    }

    case PM_CASE_NODE: {
        /* Case as expression — emit if/else chain assigning to a temp */
        pm_case_node_t *n = (pm_case_node_t *)node;
        vtype_t rt = infer_type(ctx, node);
        char *ct = vt_ctype(ctx, rt, false);
        int tmp = ctx->temp_counter++;
        char *pred = n->predicate ? codegen_expr(ctx, n->predicate) : NULL;
        int cid = ctx->temp_counter++;

        if (pred) {
            char *pct = vt_ctype(ctx, infer_type(ctx, n->predicate), false);
            emit(ctx, "%s _cpred_%d = %s;\n", pct, cid, pred);
            free(pct); free(pred);
        }
        emit(ctx, "%s _cres_%d = 0;\n", ct, tmp);

        for (size_t i = 0; i < n->conditions.size; i++) {
            pm_node_t *cn = n->conditions.nodes[i];
            if (PM_NODE_TYPE(cn) != PM_WHEN_NODE) continue;
            pm_when_node_t *w = (pm_when_node_t *)cn;
            emit(ctx, "%sif (", i == 0 ? "" : "} else ");
            for (size_t j = 0; j < w->conditions.size; j++) {
                if (j > 0) emit_raw(ctx, " || ");
                pm_node_t *wc = w->conditions.nodes[j];
                if (PM_NODE_TYPE(wc) == PM_RANGE_NODE && pred) {
                    pm_range_node_t *rng = (pm_range_node_t *)wc;
                    char *lo = codegen_expr(ctx, rng->left);
                    char *hi = codegen_expr(ctx, rng->right);
                    emit_raw(ctx, "(_cpred_%d >= %s && _cpred_%d <= %s)", cid, lo, cid, hi);
                    free(lo); free(hi);
                } else if (pred) {
                    char *val = codegen_expr(ctx, wc);
                    emit_raw(ctx, "_cpred_%d == %s", cid, val);
                    free(val);
                } else {
                    char *val = codegen_expr(ctx, wc);
                    emit_raw(ctx, "%s", val);
                    free(val);
                }
            }
            emit_raw(ctx, ") {\n");
            ctx->indent++;
            if (w->statements) {
                pm_statements_node_t *ws = (pm_statements_node_t *)w->statements;
                for (size_t si = 0; si + 1 < ws->body.size; si++)
                    codegen_stmt(ctx, ws->body.nodes[si]);
                if (ws->body.size > 0) {
                    char *val = codegen_expr(ctx, ws->body.nodes[ws->body.size - 1]);
                    emit(ctx, "_cres_%d = %s;\n", tmp, val);
                    free(val);
                }
            }
            ctx->indent--;
        }
        if (n->else_clause) {
            emit(ctx, "} else {\n");
            ctx->indent++;
            pm_else_node_t *el = (pm_else_node_t *)n->else_clause;
            if (el->statements && el->statements->body.size > 0) {
                pm_statements_node_t *es = el->statements;
                for (size_t si = 0; si + 1 < es->body.size; si++)
                    codegen_stmt(ctx, es->body.nodes[si]);
                char *val = codegen_expr(ctx, es->body.nodes[es->body.size - 1]);
                emit(ctx, "_cres_%d = %s;\n", tmp, val);
                free(val);
            }
            ctx->indent--;
        }
        if (n->conditions.size > 0) emit(ctx, "}\n");
        free(ct);
        return sfmt("_cres_%d", tmp);
    }

    case PM_CASE_MATCH_NODE: {
        /* case/in as expression — emit if/else chain with pattern matching */
        pm_case_match_node_t *n = (pm_case_match_node_t *)node;
        vtype_t rt = infer_type(ctx, node);
        char *ct = vt_ctype(ctx, rt, false);
        int tmp = ctx->temp_counter++;
        int cid = ctx->temp_counter++;
        ctx->needs_poly = true;

        if (n->predicate) {
            char *pred = codegen_expr(ctx, n->predicate);
            vtype_t pt = infer_type(ctx, n->predicate);
            if (pt.kind != SPINEL_TYPE_POLY) {
                char *boxed = poly_box_expr_vt(ctx, pt, pred);
                emit(ctx, "sp_RbValue _cmpred_%d = %s;\n", cid, boxed);
                free(boxed);
            } else {
                emit(ctx, "sp_RbValue _cmpred_%d = %s;\n", cid, pred);
            }
            free(pred);
        }
        if (rt.kind == SPINEL_TYPE_STRING)
            emit(ctx, "%s _cres_%d = \"\";\n", ct, tmp);
        else
            emit(ctx, "%s _cres_%d = 0;\n", ct, tmp);

        for (size_t i = 0; i < n->conditions.size; i++) {
            pm_node_t *cn = n->conditions.nodes[i];
            if (PM_NODE_TYPE(cn) != PM_IN_NODE) continue;
            pm_in_node_t *in = (pm_in_node_t *)cn;
            emit(ctx, "%sif (", i == 0 ? "" : "} else ");
            codegen_pattern_cond(ctx, in->pattern, cid);
            emit_raw(ctx, ") {\n");
            ctx->indent++;
            if (in->statements) {
                pm_statements_node_t *ws = (pm_statements_node_t *)in->statements;
                for (size_t si = 0; si + 1 < ws->body.size; si++)
                    codegen_stmt(ctx, ws->body.nodes[si]);
                if (ws->body.size > 0) {
                    char *val = codegen_expr(ctx, ws->body.nodes[ws->body.size - 1]);
                    emit(ctx, "_cres_%d = %s;\n", tmp, val);
                    free(val);
                }
            }
            ctx->indent--;
        }
        if (n->else_clause) {
            emit(ctx, "} else {\n");
            ctx->indent++;
            pm_else_node_t *el = (pm_else_node_t *)n->else_clause;
            if (el->statements && el->statements->body.size > 0) {
                pm_statements_node_t *es = el->statements;
                for (size_t si = 0; si + 1 < es->body.size; si++)
                    codegen_stmt(ctx, es->body.nodes[si]);
                char *val = codegen_expr(ctx, es->body.nodes[es->body.size - 1]);
                emit(ctx, "_cres_%d = %s;\n", tmp, val);
                free(val);
            }
            ctx->indent--;
        }
        if (n->conditions.size > 0) emit(ctx, "}\n");
        free(ct);
        return sfmt("_cres_%d", tmp);
    }

    case PM_LAMBDA_NODE: {
        pm_lambda_node_t *lam = (pm_lambda_node_t *)node;
        return codegen_lambda(ctx, lam);
    }

    case PM_YIELD_NODE: {
        pm_yield_node_t *yn = (pm_yield_node_t *)node;
        if (yn->arguments && yn->arguments->arguments.size > 0) {
            char *arg = codegen_expr(ctx, yn->arguments->arguments.nodes[0]);
            char *r = sfmt("_block(_block_env, %s)", arg);
            free(arg);
            return r;
        }
        return xstrdup("_block(_block_env, 0)");
    }

    case PM_SUPER_NODE: {
        /* super(args) — call parent's same-named method */
        pm_super_node_t *sn = (pm_super_node_t *)node;
        if (ctx->current_class && ctx->current_method && ctx->current_class->superclass[0]) {
            class_info_t *parent = find_class(ctx, ctx->current_class->superclass);
            if (parent) {
                int argc = sn->arguments ? (int)sn->arguments->arguments.size : 0;
                char *args = xstrdup("");
                for (int i = 0; i < argc; i++) {
                    char *a = codegen_expr(ctx, sn->arguments->arguments.nodes[i]);
                    char *na = sfmt("%s, %s", args, a);
                    free(args); free(a);
                    args = na;
                }
                char *r;
                if (strcmp(ctx->current_method->name, "initialize") == 0) {
                    /* super in initialize: call parent's initialize on self */
                    if (ctx->current_class->is_value_type)
                        r = sfmt("sp_%s_initialize(&self%s)", parent->name, args);
                    else
                        r = sfmt("sp_%s_initialize((sp_%s *)self%s)", parent->name, parent->name, args);
                } else {
                    const char *c_mname = sanitize_method(ctx->current_method->name);
                    if (ctx->current_class->is_value_type)
                        r = sfmt("sp_%s_%s(self%s)", parent->name, c_mname, args);
                    else
                        r = sfmt("sp_%s_%s((sp_%s *)self%s)", parent->name, c_mname, parent->name, args);
                }
                free(args);
                return r;
            }
        }
        return xstrdup("/* super */");
    }

    case PM_ARRAY_NODE: {
        pm_array_node_t *ary = (pm_array_node_t *)node;
        if (ctx->lambda_mode) {
            if (ary->elements.size == 0) {
                /* Empty array literal [] → sp_ValArray_new() */
                return xstrdup("sp_ValArray_new()");
            }
        }
        vtype_t ary_type = infer_type(ctx, node);

        /* Homogeneous integer array [1, 2, 3] → sp_IntArray */
        if (ary_type.kind == SPINEL_TYPE_ARRAY) {
            int tmp = ctx->temp_counter++;
            emit(ctx, "sp_IntArray *_ary_%d = sp_IntArray_new();\n", tmp);
            for (size_t i = 0; i < ary->elements.size; i++) {
                char *val = codegen_expr(ctx, ary->elements.nodes[i]);
                emit(ctx, "sp_IntArray_push(_ary_%d, %s);\n", tmp, val);
                free(val);
            }
            return sfmt("_ary_%d", tmp);
        }

        /* Homogeneous float array [1.0, 2.0, ...] → sp_FloatArray */
        if (ary_type.kind == SPINEL_TYPE_FLOAT_ARRAY) {
            int tmp = ctx->temp_counter++;
            emit(ctx, "sp_FloatArray *_ary_%d = sp_FloatArray_new();\n", tmp);
            for (size_t i = 0; i < ary->elements.size; i++) {
                char *val = codegen_expr(ctx, ary->elements.nodes[i]);
                emit(ctx, "sp_FloatArray_push(_ary_%d, %s);\n", tmp, val);
                free(val);
            }
            return sfmt("_ary_%d", tmp);
        }

        /* Homogeneous string array ["a", "b"] → sp_StrArray */
        if (ary_type.kind == SPINEL_TYPE_STR_ARRAY) {
            ctx->needs_str_split = true; /* ensures sp_StrArray is emitted */
            int tmp = ctx->temp_counter++;
            emit(ctx, "sp_StrArray *_ary_%d = sp_StrArray_new();\n", tmp);
            for (size_t i = 0; i < ary->elements.size; i++) {
                char *val = codegen_expr(ctx, ary->elements.nodes[i]);
                emit(ctx, "sp_StrArray_push(_ary_%d, %s);\n", tmp, val);
                free(val);
            }
            return sfmt("_ary_%d", tmp);
        }

        /* sp_RbArray for array literals (heterogeneous) */
        {
            ctx->needs_rb_array = true;
            ctx->needs_poly = true;
            int tmp = ctx->temp_counter++;
            emit(ctx, "sp_RbArray *_ary_%d = sp_RbArray_new();\n", tmp);
            for (size_t i = 0; i < ary->elements.size; i++) {
                pm_node_t *elem = ary->elements.nodes[i];
                char *val = codegen_expr(ctx, elem);
                vtype_t et = infer_type(ctx, elem);
                char *boxed = poly_box_expr_vt(ctx, et, val);
                emit(ctx, "sp_RbArray_push(_ary_%d, %s);\n", tmp, boxed);
                free(val); free(boxed);
            }
            return sfmt("_ary_%d", tmp);
        }
    }

    case PM_HASH_NODE: {
        pm_hash_node_t *hn = (pm_hash_node_t *)node;
        vtype_t ht = infer_type(ctx, node);
        if (ht.kind == SPINEL_TYPE_RB_HASH) {
            /* Heterogeneous hash → sp_RbHash */
            ctx->needs_rb_hash = true;
            ctx->needs_poly = true;
            int tmp = ctx->temp_counter++;
            emit(ctx, "sp_RbHash *_rh_%d = sp_RbHash_new();\n", tmp);
            for (size_t i = 0; i < hn->elements.size; i++) {
                if (PM_NODE_TYPE(hn->elements.nodes[i]) != PM_ASSOC_NODE) continue;
                pm_assoc_node_t *assoc = (pm_assoc_node_t *)hn->elements.nodes[i];
                char *key = codegen_expr(ctx, assoc->key);
                char *val = codegen_expr(ctx, assoc->value);
                vtype_t vt = infer_type(ctx, assoc->value);
                char *boxed = poly_box_expr_vt(ctx, vt, val);
                emit(ctx, "sp_RbHash_set(_rh_%d, %s, %s);\n", tmp, key, boxed);
                free(key); free(val); free(boxed);
            }
            return sfmt("_rh_%d", tmp);
        }
        /* Empty or homogeneous integer hash literal → sp_StrIntHash */
        ctx->needs_hash = true;
        ctx->needs_gc = true;
        if (hn->elements.size == 0)
            return xstrdup("sp_StrIntHash_new()");
        {
            int tmp = ctx->temp_counter++;
            emit(ctx, "sp_StrIntHash *_sh_%d = sp_StrIntHash_new();\n", tmp);
            for (size_t i = 0; i < hn->elements.size; i++) {
                if (PM_NODE_TYPE(hn->elements.nodes[i]) != PM_ASSOC_NODE) continue;
                pm_assoc_node_t *assoc = (pm_assoc_node_t *)hn->elements.nodes[i];
                char *key = codegen_expr(ctx, assoc->key);
                char *val = codegen_expr(ctx, assoc->value);
                emit(ctx, "sp_StrIntHash_set(_sh_%d, %s, %s);\n", tmp, key, val);
                free(key); free(val);
            }
            return sfmt("_sh_%d", tmp);
        }
    }

    /* Chained assignment: zr = zi = 0 — inner write used as expression */
    case PM_LOCAL_VARIABLE_WRITE_NODE: {
        pm_local_variable_write_node_t *n = (pm_local_variable_write_node_t *)node;
        char *name = cstr(ctx, n->name);
        char *cn = make_cname(name, false);
        char *val = codegen_expr(ctx, n->value);
        var_entry_t *v = var_lookup(ctx, name);
        char *r;
        if (v && v->type.kind == SPINEL_TYPE_SP_STRING) {
            vtype_t rhs_t = infer_type(ctx, n->value);
            if (rhs_t.kind == SPINEL_TYPE_STRING)
                r = sfmt("(%s = sp_String_new(%s))", cn, val);
            else
                r = sfmt("(%s = %s)", cn, val);
        } else if (v && v->type.kind == SPINEL_TYPE_POLY) {
            vtype_t rhs_t = infer_type(ctx, n->value);
            if (rhs_t.kind != SPINEL_TYPE_POLY) {
                char *boxed = poly_box_expr(rhs_t.kind, val);
                r = sfmt("(%s = %s)", cn, boxed);
                free(boxed);
            } else {
                r = sfmt("(%s = %s)", cn, val);
            }
        } else {
            r = sfmt("(%s = %s)", cn, val);
        }
        free(name); free(cn); free(val);
        return r;
    }

    case PM_REGULAR_EXPRESSION_NODE: {
        pm_regular_expression_node_t *re = (pm_regular_expression_node_t *)node;
        const uint8_t *src = pm_string_source(&re->unescaped);
        size_t len = pm_string_length(&re->unescaped);
        /* Check if this pattern is already registered */
        char pat[256];
        size_t plen = len < 255 ? len : 255;
        memcpy(pat, src, plen);
        pat[plen] = '\0';
        for (int i = 0; i < ctx->regexp_counter; i++) {
            if (strcmp(ctx->regexps[i].pattern, pat) == 0)
                return sfmt("_re_%d", ctx->regexps[i].id);
        }
        /* Register new regexp */
        int id = ctx->regexp_counter;
        if (ctx->regexp_counter < MAX_REGEXPS) {
            snprintf(ctx->regexps[ctx->regexp_counter].pattern, 256, "%s", pat);
            ctx->regexps[ctx->regexp_counter].id = id;
            ctx->regexp_counter++;
        }
        ctx->needs_regexp = true;
        return sfmt("_re_%d", id);
    }

    case PM_NUMBERED_REFERENCE_READ_NODE: {
        pm_numbered_reference_read_node_t *nr = (pm_numbered_reference_read_node_t *)node;
        return sfmt("sp_re_group(%d)", (int)nr->number);
    }

    case PM_MATCH_WRITE_NODE: {
        pm_match_write_node_t *mw = (pm_match_write_node_t *)node;
        return codegen_expr(ctx, (pm_node_t *)mw->call);
    }

    case PM_X_STRING_NODE: {
        pm_x_string_node_t *xs = (pm_x_string_node_t *)node;
        const uint8_t *src = pm_string_source(&xs->unescaped);
        size_t len = pm_string_length(&xs->unescaped);
        char *cmd = malloc(len + 1);
        memcpy(cmd, src, len);
        cmd[len] = '\0';
        char *r = sfmt("sp_backtick(\"%s\")", cmd);
        free(cmd);
        return r;
    }

    case PM_SELF_NODE:
        return xstrdup("self");

    case PM_RANGE_NODE: {
        pm_range_node_t *rng = (pm_range_node_t *)node;
        char *left = codegen_expr(ctx, rng->left);
        char *right = codegen_expr(ctx, rng->right);
        char *r = sfmt("sp_Range_new(%s, %s)", left, right);
        free(left); free(right);
        return r;
    }

    case PM_RESCUE_MODIFIER_NODE: {
        pm_rescue_modifier_node_t *rm = (pm_rescue_modifier_node_t *)node;
        vtype_t rt = infer_type(ctx, rm->expression);
        char *ct = vt_ctype(ctx, rt, false);
        int tmp = ctx->temp_counter++;
        ctx->needs_exc = true;
        emit(ctx, "%s _resc_%d;\n", ct, tmp);
        emit(ctx, "sp_exc_depth++;\n");
        emit(ctx, "if (setjmp(sp_exc_stack[sp_exc_depth - 1]) == 0) {\n");
        ctx->indent++;
        char *expr = codegen_expr(ctx, rm->expression);
        emit(ctx, "_resc_%d = %s;\n", tmp, expr);
        free(expr);
        ctx->indent--;
        emit(ctx, "    sp_exc_depth--;\n");
        emit(ctx, "} else {\n");
        ctx->indent++;
        emit(ctx, "sp_exc_depth--;\n");
        char *defval = codegen_expr(ctx, rm->rescue_expression);
        emit(ctx, "_resc_%d = %s;\n", tmp, defval);
        free(defval);
        ctx->indent--;
        emit(ctx, "}\n");
        free(ct);
        return sfmt("_resc_%d", tmp);
    }

    case PM_KEYWORD_HASH_NODE:
        return xstrdup("0 /* kwargs */");

    case PM_RETURN_NODE: {
        pm_return_node_t *ret = (pm_return_node_t *)node;
        if (ret->arguments && ret->arguments->arguments.size > 0) {
            char *val = codegen_expr(ctx, ret->arguments->arguments.nodes[0]);
            emit(ctx, "return %s;\n", val);
            free(val);
        } else {
            emit(ctx, "return 0;\n");
        }
        return xstrdup("0"); /* unreachable but needed for expression context */
    }

    case PM_LOCAL_VARIABLE_OR_WRITE_NODE: {
        pm_local_variable_or_write_node_t *n = (pm_local_variable_or_write_node_t *)node;
        char *name = cstr(ctx, n->name);
        char *cn = make_cname(name, false);
        char *val = codegen_expr(ctx, n->value);
        char *r = sfmt("(%s ? %s : (%s = %s))", cn, cn, cn, val);
        free(name); free(cn); free(val);
        return r;
    }

    case PM_LOCAL_VARIABLE_AND_WRITE_NODE: {
        pm_local_variable_and_write_node_t *n = (pm_local_variable_and_write_node_t *)node;
        char *name = cstr(ctx, n->name);
        char *cn = make_cname(name, false);
        char *val = codegen_expr(ctx, n->value);
        char *r = sfmt("(%s ? (%s = %s) : %s)", cn, cn, val, cn);
        free(name); free(cn); free(val);
        return r;
    }

    case PM_INSTANCE_VARIABLE_OR_WRITE_NODE: {
        pm_instance_variable_or_write_node_t *n = (pm_instance_variable_or_write_node_t *)node;
        char *ivname = cstr(ctx, n->name);
        const char *field = ivname + 1;
        char *val = codegen_expr(ctx, n->value);
        char *r;
        if (ctx->current_class && ctx->current_class->is_value_type)
            r = sfmt("(self.%s ? self.%s : (self.%s = %s))", field, field, field, val);
        else if (ctx->current_class)
            r = sfmt("(self->%s ? self->%s : (self->%s = %s))", field, field, field, val);
        else if (ctx->current_module)
            r = sfmt("(sp_%s_%s ? sp_%s_%s : (sp_%s_%s = %s))", ctx->current_module->name, field, ctx->current_module->name, field, ctx->current_module->name, field, val);
        else
            r = sfmt("0");
        free(ivname); free(val);
        return r;
    }

    case PM_INTERPOLATED_REGULAR_EXPRESSION_NODE:
        /* Interpolated regexp (/pattern #{expr}/) — not yet supported at
         * compile time; return NULL placeholder so callers can handle
         * gracefully. */
        return sfmt("NULL /* interp regexp */");

    default:
        fprintf(stderr, "spinel: warning: unsupported expression node type %d\n", PM_NODE_TYPE(node));
        return sfmt("0 /* unsupported expr %d */", PM_NODE_TYPE(node));
    }
}
