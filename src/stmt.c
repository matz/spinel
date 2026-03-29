/* stmt.c - Spinel AOT: statement code generation */
#define _GNU_SOURCE  /* for open_memstream */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdbool.h>
#include "codegen.h"

/* ------------------------------------------------------------------ */
/* Statement codegen                                                  */
/* ------------------------------------------------------------------ */

/* Detect print <int>.chr → putchar */
static bool try_print_chr(codegen_ctx_t *ctx, pm_call_node_t *call) {
  if (!ceq(ctx, call->name, "print")) return false;
  if (!call->arguments || call->arguments->arguments.size != 1) return false;
  pm_node_t *arg = call->arguments->arguments.nodes[0];
  if (PM_NODE_TYPE(arg) != PM_CALL_NODE) return false;
  pm_call_node_t *inner = (pm_call_node_t *)arg;
  if (!ceq(ctx, inner->name, "chr") || !inner->receiver) return false;
  if (infer_type(ctx, inner->receiver).kind != SPINEL_TYPE_INTEGER) return false;
  char *ie = codegen_expr(ctx, inner->receiver);
  emit(ctx, "putchar((int)%s);\n", ie);
  free(ie);
  return true;
}

/* Emit a pattern matching condition for case/in.
 * Writes the C condition expression (without surrounding parens) to ctx->out.
 * _cmpred_<case_id> is the sp_RbValue predicate variable. */
void codegen_pattern_cond(codegen_ctx_t *ctx, pm_node_t *pattern, int case_id) {
  switch (PM_NODE_TYPE(pattern)) {
  case PM_CONSTANT_READ_NODE: {
    /* in Integer / in String / in Float */
    pm_constant_read_node_t *cr = (pm_constant_read_node_t *)pattern;
    if (ceq(ctx, cr->name, "Integer"))
      emit_raw(ctx, "SP_IS_INT(_cmpred_%d)", case_id);
    else if (ceq(ctx, cr->name, "String"))
      emit_raw(ctx, "SP_IS_STR(_cmpred_%d)", case_id);
    else if (ceq(ctx, cr->name, "Float"))
      emit_raw(ctx, "SP_IS_DBL(_cmpred_%d)", case_id);
    else
      emit_raw(ctx, "0 /* unsupported pattern */");
    break;
  }
  case PM_INTEGER_NODE: {
    /* in 0, in 1, etc. — value match */
    pm_integer_node_t *n = (pm_integer_node_t *)pattern;
    int64_t val = (int64_t)n->value.value;
    if (n->value.negative) val = -val;
    emit_raw(ctx, "SP_IS_INT(_cmpred_%d) && sp_unbox_int(_cmpred_%d) == %lldLL", case_id, case_id, (long long)val);
    break;
  }
  case PM_FLOAT_NODE: {
    pm_float_node_t *n = (pm_float_node_t *)pattern;
    emit_raw(ctx, "SP_IS_DBL(_cmpred_%d) && sp_unbox_float(_cmpred_%d) == %.17g", case_id, case_id, n->value);
    break;
  }
  case PM_STRING_NODE: {
    pm_string_node_t *sn = (pm_string_node_t *)pattern;
    const uint8_t *src = pm_string_source(&sn->unescaped);
    size_t len = pm_string_length(&sn->unescaped);
    emit_raw(ctx, "SP_IS_STR(_cmpred_%d) && strcmp(sp_unbox_str(_cmpred_%d), \"%.*s\") == 0",
         case_id, case_id, (int)len, src);
    break;
  }
  case PM_NIL_NODE:
    emit_raw(ctx, "SP_IS_NIL(_cmpred_%d)", case_id);
    break;
  case PM_TRUE_NODE:
    emit_raw(ctx, "SP_IS_BOOL(_cmpred_%d) && sp_unbox_bool(_cmpred_%d)", case_id, case_id);
    break;
  case PM_FALSE_NODE:
    emit_raw(ctx, "SP_IS_BOOL(_cmpred_%d) && !sp_unbox_bool(_cmpred_%d)", case_id, case_id);
    break;
  case PM_ALTERNATION_PATTERN_NODE: {
    /* in true | false → OR of sub-patterns */
    pm_alternation_pattern_node_t *alt = (pm_alternation_pattern_node_t *)pattern;
    emit_raw(ctx, "(");
    codegen_pattern_cond(ctx, alt->left, case_id);
    emit_raw(ctx, ") || (");
    codegen_pattern_cond(ctx, alt->right, case_id);
    emit_raw(ctx, ")");
    break;
  }
  default:
    emit_raw(ctx, "0 /* unsupported pattern type %d */", PM_NODE_TYPE(pattern));
    break;
  }
}

/* Forward declarations */
void codegen_stmt(codegen_ctx_t *ctx, pm_node_t *node);
void codegen_stmts(codegen_ctx_t *ctx, pm_node_t *node);

/* ------------------------------------------------------------------ */
/* PM_CALL_NODE handler (extracted from codegen_stmt)                  */
/* ------------------------------------------------------------------ */
static void codegen_stmt_call(codegen_ctx_t *ctx, pm_call_node_t *call, pm_node_t *node) {

  /* require/require_relative are compile-time directives — skip at codegen */
  if (is_require_relative(ctx, node) || is_require(ctx, node))
    return;

  /* private/protected/public/extend — access modifiers and runtime
   * mixin operations are no-ops in AOT */
  {
    char *_mname = cstr(ctx, call->name);
    if (!call->receiver &&
      (strcmp(_mname, "private") == 0 ||
       strcmp(_mname, "protected") == 0 ||
       strcmp(_mname, "public") == 0 ||
       strcmp(_mname, "extend") == 0 ||
       strcmp(_mname, "define_method") == 0)) {
      free(_mname);
      return;
    }
    free(_mname);
  }

  /* print int.chr → putchar */
  if (!call->receiver && try_print_chr(ctx, call))
    return;

  char *method = cstr(ctx, call->name);

  /* $stderr.puts / $stdout.puts */
  if (call->receiver && PM_NODE_TYPE(call->receiver) == PM_GLOBAL_VARIABLE_READ_NODE &&
    strcmp(method, "puts") == 0) {
    pm_global_variable_read_node_t *gv = (pm_global_variable_read_node_t *)call->receiver;
    char *gname = cstr(ctx, gv->name);
    const char *stream = strcmp(gname, "$stderr") == 0 ? "stderr" : "stdout";
    if (call->arguments && call->arguments->arguments.size > 0) {
      char *ae = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
      emit(ctx, "fprintf(%s, \"%%s\\n\", %s);\n", stream, ae);
      free(ae);
    }
    else {
      emit(ctx, "fputc('\\n', %s);\n", stream);
    }
    free(gname); free(method);
    return;
  }

  /* Kernel#exit — also handle exit(true)→0, exit(false)→1 */
  if (!call->receiver && strcmp(method, "exit") == 0) {
    if (call->arguments && call->arguments->arguments.size > 0) {
      pm_node_t *arg0 = call->arguments->arguments.nodes[0];
      if (PM_NODE_TYPE(arg0) == PM_TRUE_NODE) {
        emit(ctx, "exit(0);\n");
      }
      else if (PM_NODE_TYPE(arg0) == PM_FALSE_NODE) {
        emit(ctx, "exit(1);\n");
      }
      else {
        char *code = codegen_expr(ctx, arg0);
        emit(ctx, "exit((int)%s);\n", code);
        free(code);
      }
    }
    else {
      emit(ctx, "exit(0);\n");
    }
    free(method);
    return;
  }

  /* Kernel#abort */
  if (!call->receiver && strcmp(method, "abort") == 0) {
    if (call->arguments && call->arguments->arguments.size > 0) {
      char *msg = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
      emit(ctx, "fprintf(stderr, \"%%s\\n\", %s); exit(1);\n", msg);
      free(msg);
    }
    else {
      emit(ctx, "exit(1);\n");
    }
    free(method);
    return;
  }

  /* Kernel#sleep */
  if (!call->receiver && strcmp(method, "sleep") == 0) {
    if (call->arguments && call->arguments->arguments.size > 0) {
      char *secs = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
      emit(ctx, "sleep((unsigned int)%s);\n", secs);
      free(secs);
    }
    else {
      emit(ctx, "pause();\n"); /* sleep forever */
    }
    free(method);
    return;
  }

  /* Kernel#warn → fprintf(stderr, ...) */
  if (!call->receiver && strcmp(method, "warn") == 0) {
    if (call->arguments && call->arguments->arguments.size > 0) {
      char *msg = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
      emit(ctx, "fprintf(stderr, \"%%s\\n\", %s);\n", msg);
      free(msg);
    }
    else {
      emit(ctx, "fputc('\\n', stderr);\n");
    }
    free(method);
    return;
  }

  /* puts: output + newline */
  if (!call->receiver && strcmp(method, "puts") == 0) {
    /* In lambda mode: puts on a StrArray variable → iterate and print each */
    if (ctx->lambda_mode && call->arguments && call->arguments->arguments.size > 0) {
      pm_node_t *arg = call->arguments->arguments.nodes[0];
      vtype_t at = infer_type(ctx, arg);
      /* If the argument type is PROC or VALUE (could be StrArray from map), handle specially */
      if (at.kind == SPINEL_TYPE_PROC || at.kind == SPINEL_TYPE_VALUE ||
        at.kind == SPINEL_TYPE_UNKNOWN) {
        char *ae = codegen_expr(ctx, arg);
        /* Emit: cast to sp_StrArray* and print each element */
        int tmp = ctx->temp_counter++;
        emit(ctx, "{ sp_StrArray *_pa_%d = (sp_StrArray *)%s;\n", tmp, ae);
        emit(ctx, " for (int _pi_%d = 0; _pi_%d < _pa_%d->len; _pi_%d++) puts(_pa_%d->data[_pi_%d]); }\n",
           tmp, tmp, tmp, tmp, tmp, tmp);
        free(ae);
        free(method);
        return;
      }
    }
    if (call->arguments && call->arguments->arguments.size > 0) {
      pm_node_t *arg = call->arguments->arguments.nodes[0];
      vtype_t at = infer_type(ctx, arg);
      if (PM_NODE_TYPE(arg) == PM_INTERPOLATED_STRING_NODE) {
        /* Generate printf for interpolated string */
        pm_interpolated_string_node_t *is = (pm_interpolated_string_node_t *)arg;
        /* Build format string and args */
        char *fmt = xstrdup("");
        char *args = xstrdup("");
        for (size_t i = 0; i < is->parts.size; i++) {
          pm_node_t *part = is->parts.nodes[i];
          if (PM_NODE_TYPE(part) == PM_STRING_NODE) {
            pm_string_node_t *sn = (pm_string_node_t *)part;
            const uint8_t *src = pm_string_source(&sn->unescaped);
            size_t len = pm_string_length(&sn->unescaped);
            size_t bufsz = strlen(fmt) + len * 4 + 16;
            char *nf = malloc(bufsz);
            int pos = snprintf(nf, bufsz, "%s", fmt);
            for (size_t j = 0; j < len; j++) {
              uint8_t c = src[j];
              if (c == '\n') pos += snprintf(nf + pos, bufsz - pos, "\\n");
              else if (c == '\\') pos += snprintf(nf + pos, bufsz - pos, "\\\\");
              else if (c == '"') pos += snprintf(nf + pos, bufsz - pos, "\\\"");
              else if (c == '%') pos += snprintf(nf + pos, bufsz - pos, "%%%%");
              else pos += snprintf(nf + pos, bufsz - pos, "%c", c);
            }
            free(fmt); fmt = nf;
          }
          else if (PM_NODE_TYPE(part) == PM_EMBEDDED_STATEMENTS_NODE) {
            pm_embedded_statements_node_t *e = (pm_embedded_statements_node_t *)part;
            if (e->statements && e->statements->body.size > 0) {
              vtype_t et = infer_type(ctx, e->statements->body.nodes[0]);
              char *eexpr = codegen_expr(ctx, e->statements->body.nodes[0]);
              char *nf;
              if (et.kind == SPINEL_TYPE_INTEGER)
                nf = sfmt("%s%%lld", fmt);
              else if (et.kind == SPINEL_TYPE_FLOAT)
                nf = sfmt("%s%%g", fmt);
              else
                nf = sfmt("%s%%s", fmt);
              free(fmt); fmt = nf;
              char *na = sfmt("%s, (long long)%s", args, eexpr);
              free(args); free(eexpr);
              args = na;
            }
          }
        }
        emit(ctx, "printf(\"%s\\n\"%s);\n", fmt, args);
        free(fmt); free(args);
      }
      else if (at.kind == SPINEL_TYPE_STRING) {
        char *ae = codegen_expr(ctx, arg);
        emit(ctx, "{ const char *_ps = %s; fputs(_ps, stdout); if (!*_ps || _ps[strlen(_ps)-1] != '\\n') putchar('\\n'); }\n", ae);
        free(ae);
      }
      else if (at.kind == SPINEL_TYPE_SP_STRING) {
        char *ae = codegen_expr(ctx, arg);
        emit(ctx, "{ const char *_ps = sp_String_cstr(%s); fputs(_ps, stdout); if (!*_ps || _ps[strlen(_ps)-1] != '\\n') putchar('\\n'); }\n", ae);
        free(ae);
      }
      else if (at.kind == SPINEL_TYPE_BOOLEAN) {
        char *ae = codegen_expr(ctx, arg);
        emit(ctx, "puts(%s ? \"true\" : \"false\");\n", ae);
        free(ae);
      }
      else if (at.kind == SPINEL_TYPE_FLOAT) {
        char *ae = codegen_expr(ctx, arg);
        emit(ctx, "{ const char *_fs = sp_float_to_s(%s); printf(\"%%s\\n\", _fs); }\n", ae);
        free(ae);
      }
      else if (at.kind == SPINEL_TYPE_POLY) {
        char *ae = codegen_expr(ctx, arg);
        emit(ctx, "sp_poly_puts(%s);\n", ae);
        free(ae);
      }
      else if (at.kind == SPINEL_TYPE_NIL) {
        emit(ctx, "putchar('\\n');\n");
      }
      else {
        char *ae = codegen_expr(ctx, arg);
        emit(ctx, "printf(\"%%lld\\n\", (long long)%s);\n", ae);
        free(ae);
      }
    }
    else {
      emit(ctx, "putchar('\\n');\n");
    }
    free(method);
    return;
  }

  /* print */
  if (!call->receiver && strcmp(method, "print") == 0) {
    if (!try_print_chr(ctx, call)) {
      if (call->arguments && call->arguments->arguments.size > 0) {
        char *ae = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
        emit(ctx, "printf(\"%%s\", %s);\n", ae);
        free(ae);
      }
    }
    free(method);
    return;
  }

  /* putc: output a single character (integer or string) */
  if (!call->receiver && strcmp(method, "putc") == 0) {
    if (call->arguments && call->arguments->arguments.size > 0) {
      pm_node_t *arg = call->arguments->arguments.nodes[0];
      vtype_t at = infer_type(ctx, arg);
      char *ae = codegen_expr(ctx, arg);
      if (at.kind == SPINEL_TYPE_INTEGER || at.kind == SPINEL_TYPE_FLOAT)
        emit(ctx, "putchar((int)%s);\n", ae);
      else if (at.kind == SPINEL_TYPE_STRING)
        emit(ctx, "putchar(%s[0]);\n", ae);
      else
        emit(ctx, "putchar((int)%s);\n", ae);
      free(ae);
    }
    free(method);
    return;
  }

  /* printf with format string */
  if (!call->receiver && strcmp(method, "printf") == 0) {
    if (call->arguments && call->arguments->arguments.size >= 1) {
      char *fmt = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
      if (call->arguments->arguments.size == 1) {
        emit(ctx, "printf(%s);\n", fmt);
      }
      else {
        emit(ctx, "printf(%s", fmt);
        for (size_t i = 1; i < call->arguments->arguments.size; i++) {
          char *a = codegen_expr(ctx, call->arguments->arguments.nodes[i]);
          emit_raw(ctx, ", %s", a);
          free(a);
        }
        emit_raw(ctx, ");\n");
      }
      free(fmt);
    }
    free(method);
    return;
  }

  /* p — debug print */
  if (!call->receiver && strcmp(method, "p") == 0) {
    if (call->arguments && call->arguments->arguments.size > 0) {
      pm_node_t *arg = call->arguments->arguments.nodes[0];
      vtype_t at = infer_type(ctx, arg);
      char *ae = codegen_expr(ctx, arg);
      if (at.kind == SPINEL_TYPE_STRING)
        emit(ctx, "{ const char *_ps = %s; fputs(_ps, stdout); if (!*_ps || _ps[strlen(_ps)-1] != '\\n') putchar('\\n'); }\n", ae);
      else
        emit(ctx, "printf(\"%%lld\\n\", (long long)%s);\n", ae);
      free(ae);
    }
    free(method);
    return;
  }

  /* srand(n) → C srand */
  if (!call->receiver && strcmp(method, "srand") == 0) {
    if (call->arguments && call->arguments->arguments.size == 1) {
      char *arg = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
      emit(ctx, "srand((unsigned int)%s);\n", arg);
      free(arg);
    }
    else {
      emit(ctx, "srand(0);\n");
    }
    free(method);
    return;
  }

  /* exit(n) → exit(n) */
  if (!call->receiver && strcmp(method, "exit") == 0) {
    if (call->arguments && call->arguments->arguments.size == 1) {
      char *arg = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
      emit(ctx, "exit((int)%s);\n", arg);
      free(arg);
    }
    else {
      emit(ctx, "exit(0);\n");
    }
    free(method);
    return;
  }

  /* sleep(n) → sleep(n) (use unistd.h on POSIX) */
  if (!call->receiver && strcmp(method, "sleep") == 0) {
    if (call->arguments && call->arguments->arguments.size == 1) {
      char *arg = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
      emit(ctx, "sleep((unsigned int)%s);\n", arg);
      free(arg);
    }
    else {
      emit(ctx, "sleep(0);\n");
    }
    free(method);
    return;
  }

  /* system("cmd") → system("cmd") as statement */
  if (!call->receiver && strcmp(method, "system") == 0) {
    if (call->arguments && call->arguments->arguments.size >= 1) {
      char *arg = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
      emit(ctx, "fflush(stdout); sp_last_status = system(%s);\n", arg);
      free(arg);
    }
    free(method);
    return;
  }

  /* trap('SIGNAL') { block } → signal(SIGxxx, SIG_IGN) */
  if (!call->receiver && strcmp(method, "trap") == 0) {
    if (call->arguments && call->arguments->arguments.size >= 1) {
      char *sig = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
      /* Map Ruby signal names to C signal constants */
      const char *csig = "SIGINT"; /* default */
      /* Extract string content (remove quotes) for comparison */
      if (strstr(sig, "INT")) csig = "SIGINT";
      else if (strstr(sig, "WINCH")) csig = "SIGWINCH";
      else if (strstr(sig, "TERM")) csig = "SIGTERM";
      else if (strstr(sig, "HUP")) csig = "SIGHUP";
      else if (strstr(sig, "PIPE")) csig = "SIGPIPE";
      else if (strstr(sig, "USR1")) csig = "SIGUSR1";
      else if (strstr(sig, "USR2")) csig = "SIGUSR2";
      emit(ctx, "signal(%s, SIG_IGN);\n", csig);
      free(sig);
    }
    free(method);
    return;
  }

  /* loop do ... end → while (1) { body } */
  if (!call->receiver && strcmp(method, "loop") == 0 &&
    call->block && PM_NODE_TYPE(call->block) == PM_BLOCK_NODE) {
    pm_block_node_t *blk = (pm_block_node_t *)call->block;
    emit(ctx, "while (1) {\n");
    ctx->indent++;
    ctx->for_depth++;
    if (blk->body) codegen_stmts(ctx, (pm_node_t *)blk->body);
    ctx->for_depth--;
    ctx->indent--;
    emit(ctx, "}\n");
    free(method);
    return;
  }

  /* report_duration(:label) { block } → just execute the block body */
  if (!call->receiver && strcmp(method, "report_duration") == 0 &&
    call->block && PM_NODE_TYPE(call->block) == PM_BLOCK_NODE) {
    pm_block_node_t *blk = (pm_block_node_t *)call->block;
    if (blk->body) codegen_stmts(ctx, (pm_node_t *)blk->body);
    free(method);
    return;
  }

  /* raise "msg" or raise ClassName, "msg" */
  if (!call->receiver && strcmp(method, "raise") == 0) {
    int argc = call->arguments ? (int)call->arguments->arguments.size : 0;
    if (argc >= 2 && PM_NODE_TYPE(call->arguments->arguments.nodes[0]) == PM_CONSTANT_READ_NODE) {
      /* raise ClassName, "message" */
      pm_constant_read_node_t *cr = (pm_constant_read_node_t *)call->arguments->arguments.nodes[0];
      char *cls = cstr(ctx, cr->name);
      char *msg = codegen_expr(ctx, call->arguments->arguments.nodes[1]);
      emit(ctx, "sp_raise_cls(\"%s\", %s);\n", cls, msg);
      free(cls); free(msg);
    }
    else if (argc >= 1) {
      char *msg = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
      emit(ctx, "sp_raise(%s);\n", msg);
      free(msg);
    }
    else {
      emit(ctx, "sp_raise(\"RuntimeError\");\n");
    }
    free(method);
    return;
  }

  /* throw :tag, value */
  if (!call->receiver && strcmp(method, "throw") == 0 &&
    call->arguments && call->arguments->arguments.size >= 1) {
    pm_node_t *tag_node = call->arguments->arguments.nodes[0];
    char *tag = codegen_expr(ctx, tag_node);
    int argc = (int)call->arguments->arguments.size;
    if (argc >= 2) {
      pm_node_t *val_node = call->arguments->arguments.nodes[1];
      vtype_t vt = infer_type(ctx, val_node);
      char *val = codegen_expr(ctx, val_node);
      if (vt.kind == SPINEL_TYPE_STRING)
        emit(ctx, "sp_throw_s(%s, %s);\n", tag, val);
      else
        emit(ctx, "sp_throw_i(%s, %s);\n", tag, val);
      free(val);
    }
    else {
      emit(ctx, "sp_throw_i(%s, 0);\n", tag);
    }
    free(tag); free(method);
    return;
  }

  /* each_with_index on Array/StrArray → for loop with index (statement context) */
  if (call->block && PM_NODE_TYPE(call->block) == PM_BLOCK_NODE &&
    strcmp(method, "each_with_index") == 0 && call->receiver) {
    vtype_t recv_t = infer_type(ctx, call->receiver);
    if (recv_t.kind == SPINEL_TYPE_ARRAY || recv_t.kind == SPINEL_TYPE_STR_ARRAY) {
      pm_block_node_t *blk = (pm_block_node_t *)call->block;
      char *recv = codegen_expr(ctx, call->receiver);
      char *bpname = NULL, *bpidx = NULL;
      if (blk->parameters && PM_NODE_TYPE(blk->parameters) == PM_BLOCK_PARAMETERS_NODE) {
        pm_block_parameters_node_t *bp = (pm_block_parameters_node_t *)blk->parameters;
        if (bp->parameters && bp->parameters->requireds.size > 0) {
          pm_node_t *p = bp->parameters->requireds.nodes[0];
          if (PM_NODE_TYPE(p) == PM_REQUIRED_PARAMETER_NODE)
            bpname = cstr(ctx, ((pm_required_parameter_node_t *)p)->name);
        }
        if (bp->parameters && bp->parameters->requireds.size > 1) {
          pm_node_t *p = bp->parameters->requireds.nodes[1];
          if (PM_NODE_TYPE(p) == PM_REQUIRED_PARAMETER_NODE)
            bpidx = cstr(ctx, ((pm_required_parameter_node_t *)p)->name);
        }
      }
      int tmp = ctx->temp_counter++;
      bool is_str = (recv_t.kind == SPINEL_TYPE_STR_ARRAY);
      const char *len_fn = is_str ? "sp_StrArray_length" : "sp_IntArray_length";
      emit(ctx, "for (mrb_int _ei_%d = 0; _ei_%d < %s(%s); _ei_%d++) {\n",
         tmp, tmp, len_fn, recv, tmp);
      ctx->indent++;
      if (bpname) {
        char *cn = make_cname(bpname, false);
        if (is_str)
          emit(ctx, "const char *%s = (%s)->data[_ei_%d];\n", cn, recv, tmp);
        else
          emit(ctx, "mrb_int %s = sp_IntArray_get(%s, _ei_%d);\n", cn, recv, tmp);
        free(cn);
      }
      if (bpidx) {
        char *cn = make_cname(bpidx, false);
        emit(ctx, "mrb_int %s = _ei_%d;\n", cn, tmp);
        free(cn);
      }
      if (blk->body) {
        bool saved_ir2 = ctx->implicit_return;
        ctx->implicit_return = false;
        codegen_stmts(ctx, (pm_node_t *)blk->body);
        ctx->implicit_return = saved_ir2;
      }
      ctx->indent--;
      emit(ctx, "}\n");
      free(recv); free(bpname); free(bpidx); free(method);
      return;
    }
  }

  /* File.open(path, mode) do |f| ... end → sp_File open/close block */
  if (call->block && PM_NODE_TYPE(call->block) == PM_BLOCK_NODE &&
    strcmp(method, "open") == 0 && call->receiver &&
    PM_NODE_TYPE(call->receiver) == PM_CONSTANT_READ_NODE) {
    pm_constant_read_node_t *cr = (pm_constant_read_node_t *)call->receiver;
    if (ceq(ctx, cr->name, "File") && call->arguments &&
      call->arguments->arguments.size >= 1) {
      ctx->needs_file_io = true;
      pm_block_node_t *blk = (pm_block_node_t *)call->block;
      char *path_arg = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
      char *mode_arg = NULL;
      if (call->arguments->arguments.size >= 2)
        mode_arg = codegen_expr(ctx, call->arguments->arguments.nodes[1]);
      else
        mode_arg = xstrdup("\"r\"");
      /* Get block parameter name */
      char *bpname = extract_block_param(ctx, blk);
      char *cn = bpname ? make_cname(bpname, false) : xstrdup("_fio");
      emit(ctx, "{\n");
      ctx->indent++;
      emit(ctx, "sp_File *%s = sp_File_open(%s, %s);\n", cn, path_arg, mode_arg);
      /* Register block param as FILE type */
      if (bpname)
        var_declare(ctx, bpname, vt_prim(SPINEL_TYPE_FILE), false);
      if (blk->body) {
        bool saved_ir = ctx->implicit_return;
        ctx->implicit_return = false;
        codegen_stmts(ctx, (pm_node_t *)blk->body);
        ctx->implicit_return = saved_ir;
      }
      emit(ctx, "sp_File_close(%s);\n", cn);
      ctx->indent--;
      emit(ctx, "}\n");
      free(path_arg); free(mode_arg); free(bpname); free(cn); free(method);
      return;
    }
  }

  /* f.each_line do |line| ... end on FILE-typed receiver */
  if (call->block && PM_NODE_TYPE(call->block) == PM_BLOCK_NODE &&
    strcmp(method, "each_line") == 0 && call->receiver) {
    vtype_t recv_t = infer_type(ctx, call->receiver);
    if (recv_t.kind == SPINEL_TYPE_FILE) {
      pm_block_node_t *blk = (pm_block_node_t *)call->block;
      char *recv = codegen_expr(ctx, call->receiver);
      char *bpname = extract_block_param(ctx, blk);
      int tmp = ctx->temp_counter++;
      emit(ctx, "{ char _buf_%d[4096];\n", tmp);
      emit(ctx, "while (fgets(_buf_%d, sizeof(_buf_%d), %s->fp)) {\n", tmp, tmp, recv);
      ctx->indent++;
      emit(ctx, "size_t _len_%d = strlen(_buf_%d);\n", tmp, tmp);
      emit(ctx, "if (_len_%d > 0 && _buf_%d[_len_%d-1] == '\\n') _buf_%d[_len_%d-1] = '\\0';\n",
         tmp, tmp, tmp, tmp, tmp);
      if (bpname) {
        char *cn = make_cname(bpname, false);
        emit(ctx, "const char *%s = _buf_%d;\n", cn, tmp);
        free(cn);
      }
      if (blk->body) {
        bool saved_ir = ctx->implicit_return;
        ctx->implicit_return = false;
        codegen_stmts(ctx, (pm_node_t *)blk->body);
        ctx->implicit_return = saved_ir;
      }
      ctx->indent--;
      emit(ctx, "}\n");
      emit(ctx, "}\n");
      free(recv); free(bpname); free(method);
      return;
    }
  }

  /* f.puts / f.write / f.flock / f.seek / f.close on FILE-typed receiver (statement) */
  if (call->receiver && (strcmp(method, "puts") == 0 || strcmp(method, "write") == 0 ||
    strcmp(method, "flock") == 0 || strcmp(method, "seek") == 0 ||
    strcmp(method, "close") == 0)) {
    vtype_t recv_t = infer_type(ctx, call->receiver);
    if (recv_t.kind == SPINEL_TYPE_FILE) {
      char *recv = codegen_expr(ctx, call->receiver);
      if (strcmp(method, "puts") == 0) {
        if (call->arguments && call->arguments->arguments.size > 0) {
          char *arg = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
          emit(ctx, "sp_File_puts(%s, %s);\n", recv, arg);
          free(arg);
        }
        else {
          emit(ctx, "fputc('\\n', %s->fp);\n", recv);
        }
      }
      else if (strcmp(method, "write") == 0 && call->arguments &&
            call->arguments->arguments.size > 0) {
        char *arg = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
        emit(ctx, "sp_File_write_str(%s, %s);\n", recv, arg);
        free(arg);
      }
      else if (strcmp(method, "flock") == 0 && call->arguments &&
            call->arguments->arguments.size > 0) {
        char *arg = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
        emit(ctx, "flock(fileno(%s->fp), %s);\n", recv, arg);
        free(arg);
      }
      else if (strcmp(method, "seek") == 0 && call->arguments &&
            call->arguments->arguments.size > 0) {
        char *arg = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
        emit(ctx, "fseek(%s->fp, (long)%s, SEEK_SET);\n", recv, arg);
        free(arg);
      }
      else if (strcmp(method, "close") == 0) {
        emit(ctx, "sp_File_close(%s);\n", recv);
      }
      free(recv); free(method);
      return;
    }
  }

  /* String#each_char with block → iterate over characters */
  if (call->block && PM_NODE_TYPE(call->block) == PM_BLOCK_NODE &&
    strcmp(method, "each_char") == 0 && call->receiver) {
    vtype_t recv_t = infer_type(ctx, call->receiver);
    if (recv_t.kind == SPINEL_TYPE_STRING || recv_t.kind == SPINEL_TYPE_SP_STRING) {
      pm_block_node_t *blk = (pm_block_node_t *)call->block;
      char *recv = codegen_expr(ctx, call->receiver);
      char *bpname = extract_block_param(ctx, blk);
      const char *cstr_expr = (recv_t.kind == SPINEL_TYPE_SP_STRING)
        ? sfmt("sp_String_cstr(%s)", recv) : recv;
      int tmp = ctx->temp_counter++;
      emit(ctx, "{ const char *_cs_%d = %s; mrb_int _cl_%d = (mrb_int)strlen(_cs_%d);\n",
         tmp, cstr_expr, tmp, tmp);
      emit(ctx, "for (mrb_int _ci_%d = 0; _ci_%d < _cl_%d; _ci_%d++) {\n",
         tmp, tmp, tmp, tmp);
      ctx->indent++;
      if (bpname) {
        char *cn = make_cname(bpname, false);
        emit(ctx, "char _cbuf_%d[2] = { _cs_%d[_ci_%d], 0 };\n", tmp, tmp, tmp);
        emit(ctx, "const char *%s = _cbuf_%d;\n", cn, tmp);
        free(cn);
      }
      if (blk->body) {
        bool saved_ir = ctx->implicit_return;
        ctx->implicit_return = false;
        codegen_stmts(ctx, (pm_node_t *)blk->body);
        ctx->implicit_return = saved_ir;
      }
      ctx->indent--;
      emit(ctx, "} }\n");
      if (recv_t.kind == SPINEL_TYPE_SP_STRING) free((char *)cstr_expr);
      free(recv); free(bpname); free(method);
      return;
    }
  }

  /* String#each_line with block → split by newline and iterate */
  if (call->block && PM_NODE_TYPE(call->block) == PM_BLOCK_NODE &&
    strcmp(method, "each_line") == 0 && call->receiver) {
    vtype_t recv_t = infer_type(ctx, call->receiver);
    if (recv_t.kind == SPINEL_TYPE_STRING) {
      pm_block_node_t *blk = (pm_block_node_t *)call->block;
      char *recv = codegen_expr(ctx, call->receiver);
      char *bpname = extract_block_param(ctx, blk);
      ctx->needs_str_split = true;
      int tmp = ctx->temp_counter++;
      emit(ctx, "{ sp_StrArray *_lines_%d = sp_str_split(%s, \"\\n\");\n", tmp, recv);
      emit(ctx, "for (mrb_int _li_%d = 0; _li_%d < sp_StrArray_length(_lines_%d); _li_%d++) {\n",
         tmp, tmp, tmp, tmp);
      ctx->indent++;
      if (bpname) {
        char *cn = make_cname(bpname, false);
        emit(ctx, "const char *%s = _lines_%d->data[_li_%d];\n", cn, tmp, tmp);
        free(cn);
      }
      if (blk->body) {
        bool saved_ir2 = ctx->implicit_return;
        ctx->implicit_return = false;
        codegen_stmts(ctx, (pm_node_t *)blk->body);
        ctx->implicit_return = saved_ir2;
      }
      ctx->indent--;
      emit(ctx, "}\n");
      emit(ctx, "}\n");
      free(recv); free(bpname); free(method);
      return;
    }
  }

  /* Array#each with block → inline for loop (statement context) */
  if (call->block && PM_NODE_TYPE(call->block) == PM_BLOCK_NODE &&
    strcmp(method, "each") == 0 && call->receiver) {
    vtype_t recv_t = infer_type(ctx, call->receiver);
    if (recv_t.kind == SPINEL_TYPE_ARRAY) {
      pm_block_node_t *blk = (pm_block_node_t *)call->block;
      char *recv = codegen_expr(ctx, call->receiver);
      char *bpname = extract_block_param(ctx, blk);
      int tmp = ctx->temp_counter++;
      emit(ctx, "for (mrb_int _ei_%d = 0; _ei_%d < sp_IntArray_length(%s); _ei_%d++) {\n",
         tmp, tmp, recv, tmp);
      ctx->indent++;
      if (bpname) {
        char *cn = make_cname(bpname, false);
        emit(ctx, "mrb_int %s = sp_IntArray_get(%s, _ei_%d);\n", cn, recv, tmp);
        free(cn);
      }
      if (blk->body) {
        bool saved_ir2 = ctx->implicit_return;
        ctx->implicit_return = false;
        codegen_stmts(ctx, (pm_node_t *)blk->body);
        ctx->implicit_return = saved_ir2;
      }
      ctx->indent--;
      emit(ctx, "}\n");
      free(recv); free(bpname); free(method);
      return;
    }

    /* sp_RbArray#each with block → inline for loop with sp_RbValue elements */
    if (recv_t.kind == SPINEL_TYPE_RB_ARRAY) {
      pm_block_node_t *blk = (pm_block_node_t *)call->block;
      char *recv = codegen_expr(ctx, call->receiver);
      char *bpname = extract_block_param(ctx, blk);
      int tmp = ctx->temp_counter++;
      emit(ctx, "for (mrb_int _ei_%d = 0; _ei_%d < sp_RbArray_length(%s); _ei_%d++) {\n",
         tmp, tmp, recv, tmp);
      ctx->indent++;
      /* Temporarily override the block param type to POLY */
      int saved_vc = ctx->var_count;
      vtype_t saved_type = {0};
      var_entry_t *existing_v = bpname ? var_lookup(ctx, bpname) : NULL;
      if (existing_v) {
        saved_type = existing_v->type;
        existing_v->type = vt_prim(SPINEL_TYPE_POLY);
      }
      else if (bpname) {
        /* Force-add a new POLY entry at the end */
        assert(ctx->var_count < MAX_VARS);
        var_entry_t *nv = &ctx->vars[ctx->var_count++];
        snprintf(nv->name, sizeof(nv->name), "%s", bpname);
        nv->type = vt_prim(SPINEL_TYPE_POLY);
        nv->declared = false;
        nv->is_constant = false;
      }
      if (bpname) {
        char *cn = make_cname(bpname, false);
        emit(ctx, "sp_RbValue %s = sp_RbArray_get(%s, _ei_%d);\n", cn, recv, tmp);
        free(cn);
      }
      if (blk->body) {
        bool saved_ir2 = ctx->implicit_return;
        ctx->implicit_return = false;
        codegen_stmts(ctx, (pm_node_t *)blk->body);
        ctx->implicit_return = saved_ir2;
      }
      /* Restore var table */
      if (existing_v) {
        existing_v->type = saved_type;
      }
      else {
        ctx->var_count = saved_vc;
      }
      ctx->indent--;
      emit(ctx, "}\n");
      free(recv); free(bpname); free(method);
      return;
    }

    /* Range#each with block |i| → inline for loop */
    if (recv_t.kind == SPINEL_TYPE_RANGE) {
      pm_block_node_t *blk = (pm_block_node_t *)call->block;
      char *recv = codegen_expr(ctx, call->receiver);
      char *bpname = extract_block_param(ctx, blk);
      int tmp = ctx->temp_counter++;
      emit(ctx, "{ sp_Range _rng_%d = %s;\n", tmp, recv);
      if (bpname) {
        char *cn = make_cname(bpname, false);
        emit(ctx, "for (mrb_int %s = _rng_%d.first; %s <= _rng_%d.last; %s++) {\n",
           cn, tmp, cn, tmp, cn);
        free(cn);
      }
      else {
        emit(ctx, "for (mrb_int _ri_%d = _rng_%d.first; _ri_%d <= _rng_%d.last; _ri_%d++) {\n",
           tmp, tmp, tmp, tmp, tmp);
      }
      ctx->indent++;
      if (blk->body) codegen_stmts(ctx, (pm_node_t *)blk->body);
      ctx->indent--;
      emit(ctx, "}\n");
      emit(ctx, "}\n");
      free(recv); free(bpname); free(method);
      return;
    }

    /* sp_StrArray#each with block → inline for loop with string elements */
    if (recv_t.kind == SPINEL_TYPE_STR_ARRAY) {
      pm_block_node_t *blk = (pm_block_node_t *)call->block;
      char *recv = codegen_expr(ctx, call->receiver);
      char *bpname = extract_block_param(ctx, blk);
      int tmp = ctx->temp_counter++;
      emit(ctx, "for (mrb_int _ei_%d = 0; _ei_%d < sp_StrArray_length(%s); _ei_%d++) {\n",
         tmp, tmp, recv, tmp);
      ctx->indent++;
      if (bpname) {
        char *cn = make_cname(bpname, false);
        emit(ctx, "const char *%s = (%s)->data[_ei_%d];\n", cn, recv, tmp);
        free(cn);
      }
      if (blk->body) {
        bool saved_ir2 = ctx->implicit_return;
        ctx->implicit_return = false;
        codegen_stmts(ctx, (pm_node_t *)blk->body);
        ctx->implicit_return = saved_ir2;
      }
      ctx->indent--;
      emit(ctx, "}\n");
      free(recv); free(bpname); free(method);
      return;
    }

    /* Object method with block → generate block callback and pass to method */
    if (recv_t.kind == SPINEL_TYPE_OBJECT) {
      class_info_t *rcls = find_class(ctx, recv_t.klass);
      if (rcls) {
        class_info_t *owner = NULL;
        method_info_t *target = find_method_inherited(ctx, rcls, method, &owner);
        if (target && target->body_node && has_yield_nodes(target->body_node)) {
          pm_block_node_t *blk = (pm_block_node_t *)call->block;
          char *recv = codegen_expr(ctx, call->receiver);

          /* Detect non-local return in block body */
          bool has_nonlocal_return = blk->body && block_has_return((pm_node_t *)blk->body);

          /* Extract block parameter name */
          char *bpname = NULL;
          if (blk->parameters && PM_NODE_TYPE(blk->parameters) == PM_BLOCK_PARAMETERS_NODE) {
            pm_block_parameters_node_t *bp = (pm_block_parameters_node_t *)blk->parameters;
            if (bp->parameters && bp->parameters->requireds.size > 0) {
              pm_node_t *p = bp->parameters->requireds.nodes[0];
              if (PM_NODE_TYPE(p) == PM_REQUIRED_PARAMETER_NODE)
                bpname = cstr(ctx, ((pm_required_parameter_node_t *)p)->name);
            }
          }

          /* Scan captures: variables used in block body (excluding block param) */
          capture_list_t local_defs = {.count = 0};
          capture_list_t captures = {.count = 0};
          scan_captures(ctx, (pm_node_t *)blk->body,
                 bpname ? bpname : "", &local_defs, &captures);

          /* Generate block callback (written to block_out) */
          int blk_id = ctx->block_counter++;
          FILE *saved_out = ctx->out;
          if (ctx->block_out) ctx->out = ctx->block_out;

          /* Env struct: captured scalars by pointer */
          emit_raw(ctx, "typedef struct { ");
          for (int ci = 0; ci < captures.count; ci++) {
            var_entry_t *v = var_lookup(ctx, captures.names[ci]);
            if (!v) continue;
            char *cn = make_cname(v->name, false);
            emit_raw(ctx, "mrb_int *%s; ", cn);
            free(cn);
          }
          if (has_nonlocal_return) {
            emit_raw(ctx, "volatile mrb_int *_blk_ret_val; int _blk_ret_depth; ");
          }
          if (captures.count == 0 && !has_nonlocal_return) emit_raw(ctx, "int _dummy; ");
          emit_raw(ctx, "} _blk_env_%d_t;\n", blk_id);

          emit_raw(ctx, "static mrb_int _blk_%d(void *_e, mrb_int _arg) {\n", blk_id);
          emit_raw(ctx, "  _blk_env_%d_t *_env = (_blk_env_%d_t *)_e;\n", blk_id, blk_id);

          /* Block parameter from _arg */
          if (bpname) {
            char *cn = make_cname(bpname, false);
            emit_raw(ctx, "  mrb_int %s = _arg;\n", cn);
            free(cn);
          }

          /* Alias captured variables: #define lv_total (*_env->lv_total) */
          for (int ci = 0; ci < captures.count; ci++) {
            var_entry_t *v = var_lookup(ctx, captures.names[ci]);
            if (!v) continue;
            char *cn = make_cname(v->name, false);
            emit_raw(ctx, "  #define %s (*_env->%s)\n", cn, cn);
            free(cn);
          }

          /* For non-local return, alias _e for the return macros */
          if (has_nonlocal_return) {
            emit_raw(ctx, "  #define _e _env\n");
          }

          /* Generate block body */
          int saved_indent = ctx->indent;
          ctx->indent = 1;
          bool saved_in_block_nonlocal = ctx->in_block_nonlocal;
          int saved_block_return_id = ctx->block_return_id;
          if (has_nonlocal_return) {
            ctx->in_block_nonlocal = true;
            ctx->block_return_id = blk_id;
          }
          if (blk->body) codegen_stmts(ctx, (pm_node_t *)blk->body);
          ctx->in_block_nonlocal = saved_in_block_nonlocal;
          ctx->block_return_id = saved_block_return_id;
          ctx->indent = saved_indent;

          if (has_nonlocal_return) {
            emit_raw(ctx, "  #undef _e\n");
          }

          /* Undefine aliases */
          for (int ci = 0; ci < captures.count; ci++) {
            var_entry_t *v = var_lookup(ctx, captures.names[ci]);
            if (!v) continue;
            char *cn = make_cname(v->name, false);
            emit_raw(ctx, "  #undef %s\n", cn);
            free(cn);
          }

          emit_raw(ctx, "  return 0;\n");
          emit_raw(ctx, "}\n");

          ctx->out = saved_out;

          if (has_nonlocal_return) {
            /* Non-local return: wrap call with setjmp/longjmp */
            emit(ctx, "{ volatile mrb_int _blk_ret_val_%d = 0;\n", blk_id);
            emit(ctx, "sp_exc_depth++;\n");
            emit(ctx, "_blk_env_%d_t _env_%d;\n", blk_id, blk_id);
            for (int ci = 0; ci < captures.count; ci++) {
              var_entry_t *v = var_lookup(ctx, captures.names[ci]);
              if (!v) continue;
              char *cn = make_cname(v->name, false);
              emit(ctx, "_env_%d.%s = &%s;\n", blk_id, cn, cn);
              free(cn);
            }
            emit(ctx, "_env_%d._blk_ret_val = &_blk_ret_val_%d;\n", blk_id, blk_id);
            emit(ctx, "_env_%d._blk_ret_depth = sp_exc_depth - 1;\n", blk_id);

            const char *c_mname = sanitize_method(method);
            class_info_t *def_cls = owner ? owner : rcls;

            emit(ctx, "if (setjmp(sp_exc_stack[sp_exc_depth - 1]) == 0) {\n");
            ctx->indent++;
            emit(ctx, "sp_%s_%s(%s, (sp_block_fn)_blk_%d, &_env_%d);\n",
               def_cls->name, c_mname, recv, blk_id, blk_id);
            emit(ctx, "sp_exc_depth--;\n");
            ctx->indent--;
            emit(ctx, "}\n");
            emit(ctx, "else {\n");
            ctx->indent++;
            emit(ctx, "sp_exc_depth--;\n");
            emit(ctx, "return _blk_ret_val_%d;\n", blk_id);
            ctx->indent--;
            emit(ctx, "}\n");
            emit(ctx, "}\n");
          }
          else {
            /* Set up the environment struct */
            emit(ctx, "{ _blk_env_%d_t _env_%d;\n", blk_id, blk_id);
            for (int ci = 0; ci < captures.count; ci++) {
              var_entry_t *v = var_lookup(ctx, captures.names[ci]);
              if (!v) continue;
              char *cn = make_cname(v->name, false);
              emit(ctx, "_env_%d.%s = &%s;\n", blk_id, cn, cn);
              free(cn);
            }

            /* Call the method with block */
            const char *c_mname = sanitize_method(method);
            class_info_t *def_cls = owner ? owner : rcls;
            emit(ctx, "sp_%s_%s(%s, (sp_block_fn)_blk_%d, &_env_%d);\n",
               def_cls->name, c_mname, recv, blk_id, blk_id);
            emit(ctx, "}\n");
          }

          free(recv); free(bpname); free(method);
          return;
        }
      }
    }

    /* sp_RbHash#each with block |k, v| → inline loop (k=string, v=sp_RbValue) */
    if (recv_t.kind == SPINEL_TYPE_RB_HASH) {
      pm_block_node_t *blk = (pm_block_node_t *)call->block;
      char *recv = codegen_expr(ctx, call->receiver);
      char *kname = NULL, *vname = NULL;
      if (blk->parameters && PM_NODE_TYPE(blk->parameters) == PM_BLOCK_PARAMETERS_NODE) {
        pm_block_parameters_node_t *bp = (pm_block_parameters_node_t *)blk->parameters;
        if (bp->parameters && bp->parameters->requireds.size > 0) {
          pm_node_t *p = bp->parameters->requireds.nodes[0];
          if (PM_NODE_TYPE(p) == PM_REQUIRED_PARAMETER_NODE)
            kname = cstr(ctx, ((pm_required_parameter_node_t *)p)->name);
        }
        if (bp->parameters && bp->parameters->requireds.size > 1) {
          pm_node_t *p = bp->parameters->requireds.nodes[1];
          if (PM_NODE_TYPE(p) == PM_REQUIRED_PARAMETER_NODE)
            vname = cstr(ctx, ((pm_required_parameter_node_t *)p)->name);
        }
      }
      int tmp = ctx->temp_counter++;
      emit(ctx, "for (sp_RbHashEntry *_rhe_%d = %s->first; _rhe_%d; _rhe_%d = _rhe_%d->order_next) {\n",
         tmp, recv, tmp, tmp, tmp);
      ctx->indent++;
      if (kname) {
        char *cn = make_cname(kname, false);
        emit(ctx, "%s = _rhe_%d->key;\n", cn, tmp);
        free(cn);
      }
      if (vname) {
        char *cn = make_cname(vname, false);
        emit(ctx, "%s = _rhe_%d->value;\n", cn, tmp);
        free(cn);
      }
      if (blk->body) codegen_stmts(ctx, (pm_node_t *)blk->body);
      ctx->indent--;
      emit(ctx, "}\n");
      free(recv); free(kname); free(vname); free(method);
      return;
    }

    /* Hash#each with block |k, v| → inline loop over insertion-order list */
    if (recv_t.kind == SPINEL_TYPE_HASH) {
      pm_block_node_t *blk = (pm_block_node_t *)call->block;
      char *recv = codegen_expr(ctx, call->receiver);
      char *kname = NULL, *vname = NULL;
      if (blk->parameters && PM_NODE_TYPE(blk->parameters) == PM_BLOCK_PARAMETERS_NODE) {
        pm_block_parameters_node_t *bp = (pm_block_parameters_node_t *)blk->parameters;
        if (bp->parameters && bp->parameters->requireds.size > 0) {
          pm_node_t *p = bp->parameters->requireds.nodes[0];
          if (PM_NODE_TYPE(p) == PM_REQUIRED_PARAMETER_NODE)
            kname = cstr(ctx, ((pm_required_parameter_node_t *)p)->name);
        }
        if (bp->parameters && bp->parameters->requireds.size > 1) {
          pm_node_t *p = bp->parameters->requireds.nodes[1];
          if (PM_NODE_TYPE(p) == PM_REQUIRED_PARAMETER_NODE)
            vname = cstr(ctx, ((pm_required_parameter_node_t *)p)->name);
        }
      }
      int tmp = ctx->temp_counter++;
      emit(ctx, "for (sp_HashEntry *_he_%d = %s->first; _he_%d; _he_%d = _he_%d->order_next) {\n",
         tmp, recv, tmp, tmp, tmp);
      ctx->indent++;
      if (kname) {
        char *cn = make_cname(kname, false);
        emit(ctx, "%s = _he_%d->key;\n", cn, tmp);
        free(cn);
      }
      if (vname) {
        char *cn = make_cname(vname, false);
        emit(ctx, "%s = _he_%d->value;\n", cn, tmp);
        free(cn);
      }
      if (blk->body) codegen_stmts(ctx, (pm_node_t *)blk->body);
      ctx->indent--;
      emit(ctx, "}\n");
      free(recv); free(kname); free(vname); free(method);
      return;
    }
  }

  /* String#scan(regexp) { |m| ... } → inline loop with onig_search */
  if (call->block && PM_NODE_TYPE(call->block) == PM_BLOCK_NODE &&
    strcmp(method, "scan") == 0 && call->receiver &&
    call->arguments && call->arguments->arguments.size == 1) {
    vtype_t recv_t = infer_type(ctx, call->receiver);
    vtype_t arg_t = infer_type(ctx, call->arguments->arguments.nodes[0]);
    if (recv_t.kind == SPINEL_TYPE_STRING && arg_t.kind == SPINEL_TYPE_REGEXP) {
      pm_block_node_t *blk = (pm_block_node_t *)call->block;
      char *recv = codegen_expr(ctx, call->receiver);
      char *pat = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
      char *bpname = extract_block_param(ctx, blk);
      int tmp = ctx->temp_counter++;
      emit(ctx, "{ /* scan */\n");
      ctx->indent++;
      emit(ctx, "const char *_ss_%d = %s;\n", tmp, recv);
      emit(ctx, "OnigRegion *_sr_%d = onig_region_new();\n", tmp);
      emit(ctx, "const OnigUChar *_se_%d = (const OnigUChar *)_ss_%d + strlen(_ss_%d);\n", tmp, tmp, tmp);
      emit(ctx, "int _sp_%d = 0;\n", tmp);
      emit(ctx, "while (_sp_%d >= 0) {\n", tmp);
      ctx->indent++;
      emit(ctx, "_sp_%d = onig_search(%s, (const OnigUChar *)_ss_%d, _se_%d,\n", tmp, pat, tmp, tmp);
      emit(ctx, "  (const OnigUChar *)_ss_%d + _sp_%d, _se_%d, _sr_%d, ONIG_OPTION_NONE);\n", tmp, tmp, tmp, tmp);
      emit(ctx, "if (_sp_%d >= 0) {\n", tmp);
      ctx->indent++;
      if (bpname) {
        char *cn = make_cname(bpname, false);
        emit(ctx, "int _ml_%d = _sr_%d->end[0] - _sr_%d->beg[0];\n", tmp, tmp, tmp);
        emit(ctx, "char *%s = (char *)malloc(_ml_%d + 1);\n", cn, tmp);
        emit(ctx, "memcpy(%s, _ss_%d + _sr_%d->beg[0], _ml_%d);\n", cn, tmp, tmp, tmp);
        emit(ctx, "%s[_ml_%d] = '\\0';\n", cn, tmp);
        free(cn);
      }
      if (blk->body) codegen_stmts(ctx, (pm_node_t *)blk->body);
      emit(ctx, "_sp_%d = _sr_%d->end[0];\n", tmp, tmp);
      ctx->indent--;
      emit(ctx, "}\n");
      ctx->indent--;
      emit(ctx, "}\n");
      emit(ctx, "onig_region_free(_sr_%d, 1);\n", tmp);
      ctx->indent--;
      emit(ctx, "}\n");
      free(recv); free(pat); free(bpname); free(method);
      return;
    }
  }

  /* Receiver-less call with block to yield function → generate callback */
  if (!call->receiver && call->block &&
    PM_NODE_TYPE(call->block) == PM_BLOCK_NODE) {
    func_info_t *fn = find_func(ctx, method);
    if (fn && fn->has_yield) {
      pm_block_node_t *blk = (pm_block_node_t *)call->block;
      int blk_id = ctx->block_counter++;

      /* Detect non-local return in block body */
      bool has_nonlocal_return = blk->body && block_has_return((pm_node_t *)blk->body);

      /* Get block parameter name */
      char *bpname = extract_block_param(ctx, blk);

      /* Scan block body for captured variables (variables from the caller's scope) */
      capture_list_t local_defs = {.count = 0};
      capture_list_t captures = {.count = 0};
      scan_captures(ctx, (pm_node_t *)blk->body,
             bpname ? bpname : "", &local_defs, &captures);

      /* Generate the block body first (to a temp buffer). Any nested blocks
       * will be written to ctx->block_out during body generation, so they
       * appear BEFORE this block in the output (correct C ordering). */
      int saved_indent = ctx->indent;
      int saved_var_count = ctx->var_count;
      ctx->indent = 1;

      /* Register block param in var table */
      if (bpname)
        var_declare(ctx, bpname, vt_prim(SPINEL_TYPE_INTEGER), false);

      char *body_processed = NULL;
      {
        char *body_buf_data = NULL;
        size_t body_buf_size = 0;
        FILE *body_buf = open_memstream(&body_buf_data, &body_buf_size);
        FILE *saved_out = ctx->out;
        ctx->out = body_buf;

        /* Set non-local return context if block has return */
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
        ctx->out = saved_out;

        /* Replace lv_CAPNAME with (*_e->CAPNAME) for captured vars */
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

      /* Now write the complete block function to block_out.
       * At this point, any nested blocks have already been written to
       * block_out, so they appear before this block (correct for C). */
      if (ctx->block_out) {
        /* Env struct */
        fprintf(ctx->block_out, "typedef struct { ");
        for (int i = 0; i < captures.count; i++)
          fprintf(ctx->block_out, "mrb_int *%s; ", captures.names[i]);
        if (has_nonlocal_return) {
          fprintf(ctx->block_out, "volatile mrb_int *_blk_ret_val; int _blk_ret_depth; ");
        }
        if (captures.count == 0 && !has_nonlocal_return) fprintf(ctx->block_out, "int _dummy; ");
        fprintf(ctx->block_out, "} _blk_%d_env;\n", blk_id);

        /* Callback function */
        fprintf(ctx->block_out, "static mrb_int _blk_%d(void *_env, mrb_int _arg) {\n", blk_id);
        fprintf(ctx->block_out, "    _blk_%d_env *_e = (_blk_%d_env *)_env;\n", blk_id, blk_id);
        if (bpname) {
          char *cn = make_cname(bpname, false);
          fprintf(ctx->block_out, "    mrb_int %s = _arg;\n", cn);
          free(cn);
        }
        if (body_processed) {
          fprintf(ctx->block_out, "%s", body_processed);
        }
        fprintf(ctx->block_out, "    return 0;\n");
        fprintf(ctx->block_out, "}\n\n");
      }
      free(body_processed);

      /* Generate the call site: env init + function call */
      if (has_nonlocal_return) {
        /* Non-local return: wrap call with setjmp/longjmp */
        emit(ctx, "{ volatile mrb_int _blk_ret_val_%d = 0;\n", blk_id);
        emit(ctx, "sp_exc_depth++;\n");
        emit(ctx, "_blk_%d_env _env_%d = { ", blk_id, blk_id);
        for (int i = 0; i < captures.count; i++) {
          char *cn = make_cname(captures.names[i], false);
          emit_raw(ctx, "&%s, ", cn);
          free(cn);
        }
        emit_raw(ctx, "&_blk_ret_val_%d, sp_exc_depth - 1 };\n", blk_id);

        emit(ctx, "if (setjmp(sp_exc_stack[sp_exc_depth - 1]) == 0) {\n");
        ctx->indent++;

        /* Build arguments */
        int argc = call->arguments ? (int)call->arguments->arguments.size : 0;
        char *args = xstrdup("");
        for (int i = 0; i < argc; i++) {
          char *a = codegen_expr(ctx, call->arguments->arguments.nodes[i]);
          char *na = sfmt("%s%s%s", args, i > 0 ? ", " : "", a);
          free(args); free(a);
          args = na;
        }
        emit(ctx, "sp_%s(%s%s(sp_block_fn)_blk_%d, &_env_%d);\n",
           fn->name, args, argc > 0 ? ", " : "", blk_id, blk_id);
        emit(ctx, "sp_exc_depth--;\n");
        free(args);

        ctx->indent--;
        emit(ctx, "}\n");
        emit(ctx, "else {\n");
        ctx->indent++;
        emit(ctx, "sp_exc_depth--;\n");
        emit(ctx, "return _blk_ret_val_%d;\n", blk_id);
        ctx->indent--;
        emit(ctx, "}\n");
        emit(ctx, "}\n");
      }
      else {
        emit(ctx, "_blk_%d_env _env_%d = { ", blk_id, blk_id);
        for (int i = 0; i < captures.count; i++) {
          char *cn = make_cname(captures.names[i], false);
          emit_raw(ctx, "%s&%s", i > 0 ? ", " : "", cn);
          free(cn);
        }
        if (captures.count == 0) emit_raw(ctx, "0");
        emit_raw(ctx, " };\n");

        /* Build arguments */
        int argc = call->arguments ? (int)call->arguments->arguments.size : 0;
        char *args = xstrdup("");
        for (int i = 0; i < argc; i++) {
          char *a = codegen_expr(ctx, call->arguments->arguments.nodes[i]);
          char *na = sfmt("%s%s%s", args, i > 0 ? ", " : "", a);
          free(args); free(a);
          args = na;
        }
        emit(ctx, "sp_%s(%s%s(sp_block_fn)_blk_%d, &_env_%d);\n",
           fn->name, args, argc > 0 ? ", " : "", blk_id, blk_id);
        free(args);
      }
      free(bpname); free(method);
      return;
    }
  }

  /* Integer#times with block → for loop */
  if (call->block && PM_NODE_TYPE(call->block) == PM_BLOCK_NODE &&
    strcmp(method, "times") == 0) {
    pm_block_node_t *blk = (pm_block_node_t *)call->block;
    char *count = codegen_expr(ctx, call->receiver);

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

    if (bpname) {
      char *cn = make_cname(bpname, false);
      emit(ctx, "for (%s = 0; %s < %s; %s++) {\n", cn, cn, count, cn);
      free(cn); free(bpname);
    }
    else {
      int tmp = ctx->temp_counter++;
      emit(ctx, "for (mrb_int _it%d = 0; _it%d < %s; _it%d++) {\n",
         tmp, tmp, count, tmp);
    }
    free(count);

    ctx->indent++;
    ctx->for_depth++;
    if (blk->body) codegen_stmts(ctx, (pm_node_t *)blk->body);
    ctx->for_depth--;
    ctx->indent--;
    emit(ctx, "}\n");
    free(method);
    return;
  }

  /* Integer#upto/downto with block → for loop */
  if (call->block && PM_NODE_TYPE(call->block) == PM_BLOCK_NODE &&
    (strcmp(method, "upto") == 0 || strcmp(method, "downto") == 0) &&
    call->arguments && call->arguments->arguments.size == 1) {
    pm_block_node_t *blk = (pm_block_node_t *)call->block;
    char *start = codegen_expr(ctx, call->receiver);
    char *end = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
    bool is_upto = strcmp(method, "upto") == 0;

    char *bpname = NULL;
    if (blk->parameters && PM_NODE_TYPE(blk->parameters) == PM_BLOCK_PARAMETERS_NODE) {
      pm_block_parameters_node_t *bp = (pm_block_parameters_node_t *)blk->parameters;
      if (bp->parameters && bp->parameters->requireds.size > 0) {
        pm_node_t *p = bp->parameters->requireds.nodes[0];
        if (PM_NODE_TYPE(p) == PM_REQUIRED_PARAMETER_NODE)
          bpname = cstr(ctx, ((pm_required_parameter_node_t *)p)->name);
      }
    }
    if (bpname) {
      char *cn = make_cname(bpname, false);
      if (is_upto)
        emit(ctx, "for (%s = %s; %s <= %s; %s++) {\n", cn, start, cn, end, cn);
      else
        emit(ctx, "for (%s = %s; %s >= %s; %s--) {\n", cn, start, cn, end, cn);
      free(cn); free(bpname);
    }
    else {
      int tmp = ctx->temp_counter++;
      if (is_upto)
        emit(ctx, "for (mrb_int _it%d = %s; _it%d <= %s; _it%d++) {\n", tmp, start, tmp, end, tmp);
      else
        emit(ctx, "for (mrb_int _it%d = %s; _it%d >= %s; _it%d--) {\n", tmp, start, tmp, end, tmp);
    }
    free(start); free(end);
    ctx->indent++;
    ctx->for_depth++;
    if (blk->body) codegen_stmts(ctx, (pm_node_t *)blk->body);
    ctx->for_depth--;
    ctx->indent--;
    emit(ctx, "}\n");
    free(method);
    return;
  }

  /* sp_String replace/clear as statement */
  if (call->receiver && PM_NODE_TYPE(call->receiver) == PM_LOCAL_VARIABLE_READ_NODE) {
    vtype_t recv_t = infer_type(ctx, call->receiver);
    if (recv_t.kind == SPINEL_TYPE_SP_STRING) {
      if (strcmp(method, "replace") == 0 && call->arguments &&
        call->arguments->arguments.size == 1) {
        pm_local_variable_read_node_t *lv = (pm_local_variable_read_node_t *)call->receiver;
        char *vn = cstr(ctx, lv->name);
        char *cn = make_cname(vn, false);
        char *arg = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
        emit(ctx, "sp_String_replace(%s, %s);\n", cn, arg);
        free(vn); free(cn); free(arg); free(method);
        return;
      }
      if (strcmp(method, "clear") == 0) {
        pm_local_variable_read_node_t *lv = (pm_local_variable_read_node_t *)call->receiver;
        char *vn = cstr(ctx, lv->name);
        char *cn = make_cname(vn, false);
        emit(ctx, "sp_String_clear(%s);\n", cn);
        free(vn); free(cn); free(method);
        return;
      }
    }
  }

  /* String << (append) as statement */
  if (call->receiver && strcmp(method, "<<") == 0 &&
    call->arguments && call->arguments->arguments.size == 1) {
    vtype_t recv_t = infer_type(ctx, call->receiver);
    if (PM_NODE_TYPE(call->receiver) == PM_LOCAL_VARIABLE_READ_NODE) {
      if (recv_t.kind == SPINEL_TYPE_SP_STRING) {
        /* Mutable string: sp_String_append(s, arg) */
        pm_local_variable_read_node_t *lv = (pm_local_variable_read_node_t *)call->receiver;
        char *vn = cstr(ctx, lv->name);
        char *cn = make_cname(vn, false);
        pm_node_t *arg_node = call->arguments->arguments.nodes[0];
        vtype_t arg_t = infer_type(ctx, arg_node);
        char *arg = codegen_expr(ctx, arg_node);
        if (arg_t.kind == SPINEL_TYPE_SP_STRING)
          emit(ctx, "sp_String_append_str(%s, %s);\n", cn, arg);
        else if (arg_t.kind == SPINEL_TYPE_INTEGER)
          emit(ctx, "sp_String_append(%s, sp_int_to_s(%s));\n", cn, arg);
        else
          emit(ctx, "sp_String_append(%s, %s);\n", cn, arg);
        free(vn); free(cn); free(arg); free(method);
        return;
      }
      if (recv_t.kind == SPINEL_TYPE_STRING) {
        /* Immutable string: reassign with concat */
        pm_local_variable_read_node_t *lv = (pm_local_variable_read_node_t *)call->receiver;
        char *vn = cstr(ctx, lv->name);
        char *cn = make_cname(vn, false);
        char *arg = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
        emit(ctx, "%s = sp_str_concat(%s, %s);\n", cn, cn, arg);
        free(vn); free(cn); free(arg); free(method);
        return;
      }
    }
    if (recv_t.kind == SPINEL_TYPE_ARRAY) {
      /* IntArray << val → sp_IntArray_push(arr, val) */
      char *recv = codegen_expr(ctx, call->receiver);
      char *arg = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
      emit(ctx, "sp_IntArray_push(%s, %s);\n", recv, arg);
      free(recv); free(arg); free(method);
      return;
    }
  }

  /* receiver.puts / receiver.print on non-FILE (unknown/VALUE) → fallback to stdout */
  if (call->receiver && (strcmp(method, "puts") == 0 || strcmp(method, "print") == 0)) {
    vtype_t recv_t = infer_type(ctx, call->receiver);
    if (recv_t.kind == SPINEL_TYPE_VALUE || recv_t.kind == SPINEL_TYPE_UNKNOWN) {
      bool is_puts = strcmp(method, "puts") == 0;
      if (call->arguments && call->arguments->arguments.size > 0) {
        char *arg = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
        if (is_puts)
          emit(ctx, "fprintf(stdout, \"%%s\\n\", %s);\n", arg);
        else
          emit(ctx, "fprintf(stdout, \"%%s\", %s);\n", arg);
        free(arg);
      }
      else {
        if (is_puts)
          emit(ctx, "fputc('\\n', stdout);\n");
        /* print with no args is a no-op */
      }
      free(method);
      return;
    }
  }

  /* General call as statement */
  {
    char *expr = codegen_expr(ctx, node);
    emit(ctx, "%s;\n", expr);
    free(expr); free(method);
  }
  return;
}

void codegen_stmt(codegen_ctx_t *ctx, pm_node_t *node) {
  if (!node) return;

  switch (PM_NODE_TYPE(node)) {
  case PM_LOCAL_VARIABLE_WRITE_NODE: {
    pm_local_variable_write_node_t *n = (pm_local_variable_write_node_t *)node;
    char *name = cstr(ctx, n->name);
    char *cn = make_cname(name, false);
    char *val = codegen_expr(ctx, n->value);
    /* Skip array_init — array vars are declared already */
    if (strstr(val, "array_init") == NULL) {
      var_entry_t *v = var_lookup(ctx, name);
      if (v && v->type.kind == SPINEL_TYPE_SP_STRING) {
        /* Wrap string literal/expr in sp_String_new() */
        vtype_t rhs_t = infer_type(ctx, n->value);
        if (rhs_t.kind == SPINEL_TYPE_STRING)
          emit(ctx, "%s = sp_String_new(%s);\n", cn, val);
        else if (rhs_t.kind == SPINEL_TYPE_SP_STRING)
          emit(ctx, "%s = %s;\n", cn, val);
        else
          emit(ctx, "%s = sp_String_new(%s);\n", cn, val);
      }
      else if (v && v->type.kind == SPINEL_TYPE_POLY) {
        vtype_t rhs_t = infer_type(ctx, n->value);
        if (rhs_t.kind != SPINEL_TYPE_POLY) {
          char *boxed = poly_box_expr(rhs_t.kind, val);
          emit(ctx, "%s = %s;\n", cn, boxed);
          free(boxed);
        }
        else {
          emit(ctx, "%s = %s;\n", cn, val);
        }
      }
      else {
        emit(ctx, "%s = %s;\n", cn, val);
      }
    }
    free(name); free(cn); free(val);
    break;
  }

  case PM_LOCAL_VARIABLE_OR_WRITE_NODE: {
    pm_local_variable_or_write_node_t *n =
      (pm_local_variable_or_write_node_t *)node;
    char *name = cstr(ctx, n->name);
    char *cn = make_cname(name, false);
    char *val = codegen_expr(ctx, n->value);
    emit(ctx, "if (!%s) %s = %s;\n", cn, cn, val);
    free(name); free(cn); free(val);
    break;
  }

  case PM_LOCAL_VARIABLE_AND_WRITE_NODE: {
    pm_local_variable_and_write_node_t *n =
      (pm_local_variable_and_write_node_t *)node;
    char *name = cstr(ctx, n->name);
    char *cn = make_cname(name, false);
    char *val = codegen_expr(ctx, n->value);
    emit(ctx, "if (%s) %s = %s;\n", cn, cn, val);
    free(name); free(cn); free(val);
    break;
  }

  case PM_LOCAL_VARIABLE_OPERATOR_WRITE_NODE: {
    pm_local_variable_operator_write_node_t *n =
      (pm_local_variable_operator_write_node_t *)node;
    char *name = cstr(ctx, n->name);
    char *cn = make_cname(name, false);
    char *op = cstr(ctx, n->binary_operator);
    char *val = codegen_expr(ctx, n->value);
    vtype_t vt = infer_type(ctx, (pm_node_t *)n);
    if (strcmp(op, "+") == 0 && (vt.kind == SPINEL_TYPE_STRING || vt.kind == SPINEL_TYPE_SP_STRING)) {
      emit(ctx, "%s = sp_str_concat(%s, %s);\n", cn, cn, val);
    }
    else if (strcmp(op, "+") == 0 && vt.kind == SPINEL_TYPE_ARRAY) {
      /* IntArray += → concat arrays (push all elements) */
      int tmp = ctx->temp_counter++;
      emit(ctx, "{ sp_IntArray *_src_%d = %s; for (mrb_int _ci_%d = 0; _ci_%d < sp_IntArray_length(_src_%d); _ci_%d++) sp_IntArray_push(%s, sp_IntArray_get(_src_%d, _ci_%d)); }\n",
         tmp, val, tmp, tmp, tmp, tmp, cn, tmp, tmp);
    }
    else {
      emit(ctx, "%s %s= %s;\n", cn, op, val);
    }
    free(name); free(cn); free(op); free(val);
    break;
  }

  case PM_CONSTANT_WRITE_NODE: {
    pm_constant_write_node_t *n = (pm_constant_write_node_t *)node;
    char *name = cstr(ctx, n->name);
    if (!find_class(ctx, name)) {
      char *cn = make_cname(name, true);
      char *val = codegen_expr(ctx, n->value);
      emit(ctx, "%s = %s;\n", cn, val);
      free(cn); free(val);
    }
    free(name);
    break;
  }

  case PM_INSTANCE_VARIABLE_WRITE_NODE: {
    pm_instance_variable_write_node_t *n = (pm_instance_variable_write_node_t *)node;
    char *ivname = cstr(ctx, n->name);
    const char *field = ivname + 1;
    char *val = codegen_expr(ctx, n->value);
    if (ctx->current_module)
      emit(ctx, "sp_%s_%s = %s;\n", ctx->current_module->name, field, val);
    else if (ctx->current_class && ctx->current_class->is_value_type)
      emit(ctx, "self.%s = %s;\n", field, val);
    else
      emit(ctx, "self->%s = %s;\n", field, val);
    free(ivname); free(val);
    break;
  }

  case PM_INSTANCE_VARIABLE_OR_WRITE_NODE: {
    pm_instance_variable_or_write_node_t *n = (pm_instance_variable_or_write_node_t *)node;
    char *ivname = cstr(ctx, n->name);
    const char *field = ivname + 1;
    char *val = codegen_expr(ctx, n->value);
    if (ctx->current_class && ctx->current_class->is_value_type)
      emit(ctx, "if (!self.%s) self.%s = %s;\n", field, field, val);
    else if (ctx->current_class)
      emit(ctx, "if (!self->%s) self->%s = %s;\n", field, field, val);
    else if (ctx->current_module)
      emit(ctx, "if (!sp_%s_%s) sp_%s_%s = %s;\n", ctx->current_module->name, field, ctx->current_module->name, field, val);
    free(ivname); free(val);
    break;
  }

  case PM_INSTANCE_VARIABLE_OPERATOR_WRITE_NODE: {
    pm_instance_variable_operator_write_node_t *n =
      (pm_instance_variable_operator_write_node_t *)node;
    char *ivname = cstr(ctx, n->name);
    const char *field = ivname + 1;  /* skip @ */
    char *op = cstr(ctx, n->binary_operator);
    char *val = codegen_expr(ctx, n->value);
    vtype_t vt = infer_type(ctx, (pm_node_t *)n);
    if (strcmp(op, "+") == 0 && (vt.kind == SPINEL_TYPE_STRING || vt.kind == SPINEL_TYPE_SP_STRING)) {
      /* String += → sp_str_concat */
      if (ctx->current_module)
        emit(ctx, "sp_%s_%s = sp_str_concat(sp_%s_%s, %s);\n",
           ctx->current_module->name, field, ctx->current_module->name, field, val);
      else if (ctx->current_class && ctx->current_class->is_value_type)
        emit(ctx, "self.%s = sp_str_concat(self.%s, %s);\n", field, field, val);
      else if (ctx->current_class)
        emit(ctx, "self->%s = sp_str_concat(self->%s, %s);\n", field, field, val);
    }
    else {
      if (ctx->current_module)
        emit(ctx, "sp_%s_%s %s= %s;\n", ctx->current_module->name, field, op, val);
      else if (ctx->current_class && ctx->current_class->is_value_type)
        emit(ctx, "self.%s %s= %s;\n", field, op, val);
      else if (ctx->current_class)
        emit(ctx, "self->%s %s= %s;\n", field, op, val);
    }
    free(ivname); free(op); free(val);
    break;
  }

  case PM_INDEX_OR_WRITE_NODE: {
    /* h[k] ||= v → if (!h[k]) h[k] = v */
    pm_index_or_write_node_t *n = (pm_index_or_write_node_t *)node;
    if (n->arguments && n->arguments->arguments.size == 1) {
      vtype_t recv_t = infer_type(ctx, n->receiver);
      char *recv = codegen_expr(ctx, n->receiver);
      char *key = codegen_expr(ctx, n->arguments->arguments.nodes[0]);
      char *val = codegen_expr(ctx, n->value);
      if (recv_t.kind == SPINEL_TYPE_HASH) {
        emit(ctx, "if (!sp_StrIntHash_get(%s, %s)) sp_StrIntHash_set(%s, %s, %s);\n",
           recv, key, recv, key, val);
      }
      else if (recv_t.kind == SPINEL_TYPE_RB_HASH) {
        emit(ctx, "if (sp_RbHash_get(%s, %s).tag == SP_T_NIL) sp_RbHash_set(%s, %s, %s);\n",
           recv, key, recv, key, val);
      }
      free(recv); free(key); free(val);
    }
    break;
  }

  case PM_WHILE_NODE: {
    pm_while_node_t *n = (pm_while_node_t *)node;
    char *cond = codegen_expr(ctx, n->predicate);
    emit(ctx, "while (%s) {\n", cond);
    free(cond);
    ctx->indent++;
    ctx->for_depth++;
    if (n->statements) codegen_stmts(ctx, (pm_node_t *)n->statements);
    ctx->for_depth--;
    ctx->indent--;
    emit(ctx, "}\n");
    break;
  }

  case PM_UNTIL_NODE: {
    pm_until_node_t *n = (pm_until_node_t *)node;
    char *cond = codegen_expr(ctx, n->predicate);
    emit(ctx, "while (!(%s)) {\n", cond);
    free(cond);
    ctx->indent++;
    ctx->for_depth++;
    if (n->statements) codegen_stmts(ctx, (pm_node_t *)n->statements);
    ctx->for_depth--;
    ctx->indent--;
    emit(ctx, "}\n");
    break;
  }

  case PM_FOR_NODE: {
    pm_for_node_t *fn = (pm_for_node_t *)node;
    /* for i in start..end → for (i = start; i <= end; i++) */
    if (PM_NODE_TYPE(fn->index) == PM_LOCAL_VARIABLE_TARGET_NODE &&
      PM_NODE_TYPE(fn->collection) == PM_RANGE_NODE) {
      pm_local_variable_target_node_t *t =
        (pm_local_variable_target_node_t *)fn->index;
      pm_range_node_t *rng = (pm_range_node_t *)fn->collection;
      char *vname = cstr(ctx, t->name);
      char *cn = make_cname(vname, false);
      char *lo = codegen_expr(ctx, rng->left);
      char *hi = codegen_expr(ctx, rng->right);
      bool exclude_end = PM_NODE_FLAG_P(rng, PM_RANGE_FLAGS_EXCLUDE_END);
      emit(ctx, "for (%s = %s; %s %s %s; %s++) {\n",
         cn, lo, cn, exclude_end ? "<" : "<=", hi, cn);
      free(lo); free(hi);
      ctx->indent++;
      ctx->for_depth++;
      if (fn->statements) codegen_stmts(ctx, (pm_node_t *)fn->statements);
      ctx->for_depth--;
      ctx->indent--;
      emit(ctx, "}\n");
      free(vname); free(cn);
    }
    break;
  }

  case PM_IF_NODE: {
    pm_if_node_t *n = (pm_if_node_t *)node;
    char *cond = codegen_expr(ctx, n->predicate);
    emit(ctx, "if (%s) {\n", cond);
    free(cond);
    ctx->indent++;
    if (n->statements) codegen_stmts(ctx, (pm_node_t *)n->statements);
    ctx->indent--;
    if (n->subsequent) {
      if (PM_NODE_TYPE(n->subsequent) == PM_IF_NODE) {
        pm_if_node_t *ei = (pm_if_node_t *)n->subsequent;
        char *ec = codegen_expr(ctx, ei->predicate);
        emit(ctx, "}\n");
        emit(ctx, "else if (%s) {\n", ec);
        free(ec);
        ctx->indent++;
        if (ei->statements) codegen_stmts(ctx, (pm_node_t *)ei->statements);
        ctx->indent--;
        if (ei->subsequent) {
          emit(ctx, "}\n");
          emit(ctx, "else {\n");
          ctx->indent++;
          codegen_stmt(ctx, (pm_node_t *)ei->subsequent);
          ctx->indent--;
        }
        emit(ctx, "}\n");
      }
      else if (PM_NODE_TYPE(n->subsequent) == PM_ELSE_NODE) {
        emit(ctx, "}\n");
        emit(ctx, "else {\n");
        ctx->indent++;
        pm_else_node_t *el = (pm_else_node_t *)n->subsequent;
        if (el->statements) codegen_stmts(ctx, (pm_node_t *)el->statements);
        ctx->indent--;
        emit(ctx, "}\n");
      }
      else {
        emit(ctx, "}\n");
      }
    }
    else {
      emit(ctx, "}\n");
    }
    break;
  }

  case PM_BREAK_NODE:
    emit(ctx, "break;\n");
    break;

  case PM_NEXT_NODE:
    emit(ctx, "continue;\n");
    break;

  case PM_UNLESS_NODE: {
    pm_unless_node_t *n = (pm_unless_node_t *)node;
    char *cond = codegen_expr(ctx, n->predicate);
    emit(ctx, "if (!(%s)) {\n", cond);
    free(cond);
    ctx->indent++;
    if (n->statements) codegen_stmts(ctx, (pm_node_t *)n->statements);
    ctx->indent--;
    if (n->else_clause) {
      emit(ctx, "}\n");
      emit(ctx, "else {\n");
      ctx->indent++;
      codegen_stmt(ctx, (pm_node_t *)n->else_clause);
      ctx->indent--;
    }
    emit(ctx, "}\n");
    break;
  }

  case PM_CASE_NODE: {
    pm_case_node_t *n = (pm_case_node_t *)node;
    char *pred = n->predicate ? codegen_expr(ctx, n->predicate) : NULL;
    int case_id = ctx->temp_counter++;

    if (pred) {
      vtype_t pt = n->predicate ? infer_type(ctx, n->predicate) : vt_prim(SPINEL_TYPE_VALUE);
      /* Declare temp for predicate to avoid re-evaluation */
      char *ct = vt_ctype(ctx, pt, false);
      emit(ctx, "%s _case_%d = %s;\n", ct, case_id, pred);
      free(ct); free(pred);
    }

    for (size_t i = 0; i < n->conditions.size; i++) {
      pm_node_t *cond_node = n->conditions.nodes[i];
      if (PM_NODE_TYPE(cond_node) != PM_WHEN_NODE) continue;
      pm_when_node_t *when = (pm_when_node_t *)cond_node;

      if (i > 0) emit(ctx, "}\n");
      emit(ctx, i == 0 ? "if (" : "else if (");

      /* Each when can have multiple conditions: when 2, 3 */
      for (size_t j = 0; j < when->conditions.size; j++) {
        if (j > 0) emit_raw(ctx, " || ");
        pm_node_t *wc = when->conditions.nodes[j];

        if (PM_NODE_TYPE(wc) == PM_RANGE_NODE && pred) {
          /* when 4..6 → _case >= 4 && _case <= 6 */
          pm_range_node_t *rng = (pm_range_node_t *)wc;
          char *lo = codegen_expr(ctx, rng->left);
          char *hi = codegen_expr(ctx, rng->right);
          emit_raw(ctx, "(_case_%d >= %s && _case_%d <= %s)", case_id, lo, case_id, hi);
          free(lo); free(hi);
        }
        else if (pred) {
          /* when value → _case == value (use strcmp for strings) */
          vtype_t pt = infer_type(ctx, n->predicate);
          char *val = codegen_expr(ctx, wc);
          if (pt.kind == SPINEL_TYPE_STRING) {
            emit_raw(ctx, "strcmp(_case_%d, %s) == 0", case_id, val);
          }
          else {
            emit_raw(ctx, "_case_%d == %s", case_id, val);
          }
          free(val);
        }
        else {
          /* case without predicate: when condition → if condition */
          char *val = codegen_expr(ctx, wc);
          emit_raw(ctx, "%s", val);
          free(val);
        }
      }
      emit_raw(ctx, ") {\n");
      ctx->indent++;
      if (when->statements) codegen_stmts(ctx, (pm_node_t *)when->statements);
      ctx->indent--;
    }

    if (n->else_clause) {
      emit(ctx, "}\n");
      emit(ctx, "else {\n");
      ctx->indent++;
      pm_else_node_t *el = (pm_else_node_t *)n->else_clause;
      if (el->statements) codegen_stmts(ctx, (pm_node_t *)el->statements);
      ctx->indent--;
    }
    if (n->conditions.size > 0)
      emit(ctx, "}\n");
    break;
  }

  case PM_CASE_MATCH_NODE: {
    pm_case_match_node_t *n = (pm_case_match_node_t *)node;
    char *pred = n->predicate ? codegen_expr(ctx, n->predicate) : NULL;
    int case_id = ctx->temp_counter++;
    ctx->needs_poly = true;

    if (pred) {
      vtype_t pt = infer_type(ctx, n->predicate);
      if (pt.kind != SPINEL_TYPE_POLY) {
        /* Box predicate to sp_RbValue for pattern matching */
        char *boxed = poly_box_expr_vt(ctx, pt, pred);
        emit(ctx, "sp_RbValue _cmpred_%d = %s;\n", case_id, boxed);
        free(boxed);
      }
      else {
        emit(ctx, "sp_RbValue _cmpred_%d = %s;\n", case_id, pred);
      }
      free(pred);
    }

    for (size_t i = 0; i < n->conditions.size; i++) {
      pm_node_t *cn = n->conditions.nodes[i];
      if (PM_NODE_TYPE(cn) != PM_IN_NODE) continue;
      pm_in_node_t *in = (pm_in_node_t *)cn;

      if (i > 0) emit(ctx, "}\n");
      emit(ctx, i == 0 ? "if (" : "else if (");
      codegen_pattern_cond(ctx, in->pattern, case_id);
      emit_raw(ctx, ") {\n");
      ctx->indent++;
      if (in->statements) codegen_stmts(ctx, (pm_node_t *)in->statements);
      ctx->indent--;
    }

    if (n->else_clause) {
      emit(ctx, "}\n");
      emit(ctx, "else {\n");
      ctx->indent++;
      pm_else_node_t *el = (pm_else_node_t *)n->else_clause;
      if (el->statements) codegen_stmts(ctx, (pm_node_t *)el->statements);
      ctx->indent--;
    }
    if (n->conditions.size > 0)
      emit(ctx, "}\n");
    break;
  }

  case PM_RETURN_NODE: {
    pm_return_node_t *n = (pm_return_node_t *)node;
    if (ctx->in_block_nonlocal) {
      /* Non-local return from block: store value and longjmp to caller */
      if (n->arguments && n->arguments->arguments.size > 0) {
        char *val = codegen_expr(ctx, n->arguments->arguments.nodes[0]);
        emit(ctx, "*_e->_blk_ret_val = %s;\n", val);
        free(val);
      }
      else {
        emit(ctx, "*_e->_blk_ret_val = 0;\n");
      }
      emit(ctx, "longjmp(sp_exc_stack[_e->_blk_ret_depth], 3);\n");
    }
    else if (n->arguments && n->arguments->arguments.size > 0) {
      char *val = codegen_expr(ctx, n->arguments->arguments.nodes[0]);
      if (ctx->gc_scope_active)
        emit(ctx, "{ SP_GC_RESTORE(); return %s; }\n", val);
      else
        emit(ctx, "return %s;\n", val);
      free(val);
    }
    else {
      if (ctx->gc_scope_active)
        emit(ctx, "{ SP_GC_RESTORE(); return; }\n");
      else
        emit(ctx, "return;\n");
    }
    break;
  }

  case PM_YIELD_NODE: {
    pm_yield_node_t *yn = (pm_yield_node_t *)node;
    if (yn->arguments && yn->arguments->arguments.size > 0) {
      char *arg = codegen_expr(ctx, yn->arguments->arguments.nodes[0]);
      emit(ctx, "_block(_block_env, %s);\n", arg);
      free(arg);
    }
    else {
      emit(ctx, "_block(_block_env, 0);\n");
    }
    break;
  }

  case PM_CALL_NODE:
    codegen_stmt_call(ctx, (pm_call_node_t *)node, node);
    break;


  case PM_ELSE_NODE: {
    pm_else_node_t *n = (pm_else_node_t *)node;
    if (n->statements) codegen_stmts(ctx, (pm_node_t *)n->statements);
    break;
  }

  case PM_BEGIN_NODE: {
    pm_begin_node_t *bn = (pm_begin_node_t *)node;
    pm_rescue_node_t *rescue = bn->rescue_clause;
    pm_ensure_node_t *ensure = bn->ensure_clause;

    if (!rescue && !ensure) {
      /* Plain begin...end with no rescue/ensure — just emit body */
      if (bn->statements) codegen_stmts(ctx, (pm_node_t *)bn->statements);
      break;
    }

    int exc_id = ctx->exc_counter++;

    if (rescue) {
      /* Check if rescue body contains retry (simple scan) */
      bool has_retry = false;
      if (rescue->statements) {
        pm_statements_node_t *rs = rescue->statements;
        for (size_t ri = 0; ri < rs->body.size; ri++) {
          pm_node_t *rn = rs->body.nodes[ri];
          if (PM_NODE_TYPE(rn) == PM_RETRY_NODE) { has_retry = true; break; }
          /* Also check modifier-if: retry if cond */
          if (PM_NODE_TYPE(rn) == PM_IF_NODE) {
            pm_if_node_t *ifn = (pm_if_node_t *)rn;
            if (ifn->statements) {
              pm_statements_node_t *ifs = ifn->statements;
              for (size_t ii = 0; ii < ifs->body.size; ii++)
                if (PM_NODE_TYPE(ifs->body.nodes[ii]) == PM_RETRY_NODE)
                  { has_retry = true; break; }
            }
          }
        }
      }
      /* Only emit retry label if rescue body uses retry */
      emit(ctx, "/* begin/rescue */\n");
      if (has_retry)
        emit_raw(ctx, "_sp_retry_%d: ;\n", exc_id);

      emit(ctx, "sp_exc_depth++;\n");
      emit(ctx, "if (setjmp(sp_exc_stack[sp_exc_depth - 1]) == 0) {\n");
      ctx->indent++;

      /* begin body */
      if (bn->statements) codegen_stmts(ctx, (pm_node_t *)bn->statements);

      ctx->indent--;
      emit(ctx, "  sp_exc_depth--;\n");
      emit(ctx, "}\n");
      emit(ctx, "else {\n");
      ctx->indent++;
      emit(ctx, "sp_exc_depth--;\n");

      /* rescue clauses — may be chained (rescue A => e; rescue B => e2) */
      pm_rescue_node_t *rc = rescue;
      bool first_rc = true;
      while (rc) {
        /* Check if this rescue has exception class filters */
        bool has_class_filter = (rc->exceptions.size > 0);

        if (has_class_filter) {
          if (!first_rc) emit(ctx, "}\n");
          emit(ctx, first_rc ? "if (" : "else if (");
          for (size_t ei = 0; ei < rc->exceptions.size; ei++) {
            if (ei > 0) emit_raw(ctx, " || ");
            pm_node_t *exc = rc->exceptions.nodes[ei];
            if (PM_NODE_TYPE(exc) == PM_CONSTANT_READ_NODE) {
              pm_constant_read_node_t *cr = (pm_constant_read_node_t *)exc;
              char *cls = cstr(ctx, cr->name);
              emit_raw(ctx, "sp_exc_is_a(\"%s\")", cls);
              free(cls);
            }
          }
          emit_raw(ctx, ") {\n");
          ctx->indent++;
        }
        else if (!first_rc) {
          emit(ctx, "}\n");
          emit(ctx, "else {\n");
          ctx->indent++;
        }

        /* rescue => e binding */
        if (rc->reference) {
          if (PM_NODE_TYPE(rc->reference) == PM_LOCAL_VARIABLE_TARGET_NODE) {
            pm_local_variable_target_node_t *t =
              (pm_local_variable_target_node_t *)rc->reference;
            char *vn = cstr(ctx, t->name);
            char *cn = make_cname(vn, false);
            emit(ctx, "%s = sp_exc_message;\n", cn);
            free(vn); free(cn);
          }
        }

        /* rescue body */
        if (rc->statements) codegen_stmts(ctx, (pm_node_t *)rc->statements);

        if (has_class_filter) ctx->indent--;

        first_rc = false;
        rc = rc->subsequent;
      }
      if (!first_rc && rescue->exceptions.size > 0)
        emit(ctx, "}\n");

      ctx->indent--;
      emit(ctx, "}\n");

    }
    else {
      /* ensure without rescue — just emit body */
      if (bn->statements) codegen_stmts(ctx, (pm_node_t *)bn->statements);
    }

    /* ensure clause */
    if (ensure && ensure->statements)
      codegen_stmts(ctx, (pm_node_t *)ensure->statements);

    break;
  }

  case PM_RETRY_NODE: {
    /* Find the nearest enclosing begin/rescue exc_id.
     * We use the current exc_counter - 1 since the enclosing begin just incremented it. */
    emit(ctx, "goto _sp_retry_%d;\n", ctx->exc_counter - 1);
    break;
  }

  case PM_RESCUE_MODIFIER_NODE: {
    /* expr rescue default_expr  → inline rescue */
    pm_rescue_modifier_node_t *rm = (pm_rescue_modifier_node_t *)node;
    int exc_id = ctx->exc_counter++;
    emit(ctx, "sp_exc_depth++;\n");
    emit(ctx, "if (setjmp(sp_exc_stack[sp_exc_depth - 1]) == 0) {\n");
    ctx->indent++;
    codegen_stmt(ctx, rm->expression);
    ctx->indent--;
    emit(ctx, "  sp_exc_depth--;\n");
    emit(ctx, "}\n");
    emit(ctx, "else {\n");
    ctx->indent++;
    emit(ctx, "sp_exc_depth--;\n");
    codegen_stmt(ctx, rm->rescue_expression);
    ctx->indent--;
    emit(ctx, "}\n");
    (void)exc_id;
    break;
  }

  case PM_MULTI_WRITE_NODE: {
    pm_multi_write_node_t *n = (pm_multi_write_node_t *)node;
    if (PM_NODE_TYPE(n->value) == PM_ARRAY_NODE) {
      pm_array_node_t *ary = (pm_array_node_t *)n->value;
      size_t count = n->lefts.size < ary->elements.size ? n->lefts.size : ary->elements.size;
      emit(ctx, "{\n"); ctx->indent++;
      for (size_t i = 0; i < count; i++) {
        vtype_t rt = infer_type(ctx, ary->elements.nodes[i]);
        char *ct = vt_ctype(ctx, rt, false);
        char *rhs = codegen_expr(ctx, ary->elements.nodes[i]);
        emit(ctx, "%s _mw_%d = %s;\n", ct, (int)i, rhs);
        free(ct); free(rhs);
      }
      for (size_t i = 0; i < count; i++) {
        if (PM_NODE_TYPE(n->lefts.nodes[i]) == PM_LOCAL_VARIABLE_TARGET_NODE) {
          pm_local_variable_target_node_t *t = (pm_local_variable_target_node_t *)n->lefts.nodes[i];
          char *vn = cstr(ctx, t->name);
          char *cn = make_cname(vn, false);
          emit(ctx, "%s = _mw_%d;\n", cn, (int)i);
          free(vn); free(cn);
        }
        else if (PM_NODE_TYPE(n->lefts.nodes[i]) == PM_INSTANCE_VARIABLE_TARGET_NODE) {
          pm_instance_variable_target_node_t *t = (pm_instance_variable_target_node_t *)n->lefts.nodes[i];
          char *ivn = cstr(ctx, t->name);
          const char *field = ivn + 1;
          if (ctx->current_module)
            emit(ctx, "sp_%s_%s = _mw_%d;\n", ctx->current_module->name, field, (int)i);
          else if (ctx->current_class && ctx->current_class->is_value_type)
            emit(ctx, "self.%s = _mw_%d;\n", field, (int)i);
          else
            emit(ctx, "self->%s = _mw_%d;\n", field, (int)i);
          free(ivn);
        }
      }
      ctx->indent--; emit(ctx, "}\n");
    }
    else {
      /* RHS is a variable or expression — evaluate once, unpack via sp_IntArray_get */
      vtype_t rhs_type = infer_type(ctx, n->value);
      char *rhs_expr = codegen_expr(ctx, n->value);
      int mv_id = ctx->temp_counter++;
      const char *arr_type = "sp_IntArray";
      const char *get_fn = "sp_IntArray_get";
      const char *len_fn = "sp_IntArray_length";
      if (rhs_type.kind == SPINEL_TYPE_FLOAT_ARRAY) {
        arr_type = "sp_FloatArray";
        get_fn = "sp_FloatArray_get";
        len_fn = "sp_FloatArray_length";
      }
      emit(ctx, "{ %s *_mv_%d = %s;\n", arr_type, mv_id, rhs_expr);
      ctx->indent++;
      for (size_t i = 0; i < n->lefts.size; i++) {
        if (PM_NODE_TYPE(n->lefts.nodes[i]) == PM_LOCAL_VARIABLE_TARGET_NODE) {
          pm_local_variable_target_node_t *t = (pm_local_variable_target_node_t *)n->lefts.nodes[i];
          char *vn = cstr(ctx, t->name);
          char *cn = make_cname(vn, false);
          emit(ctx, "%s = %s(_mv_%d, %d);\n", cn, get_fn, mv_id, (int)i);
          free(vn); free(cn);
        }
      }
      /* Handle rest (*rest) */
      if (n->rest && PM_NODE_TYPE(n->rest) == PM_SPLAT_NODE) {
        pm_splat_node_t *spl = (pm_splat_node_t *)n->rest;
        if (spl->expression && PM_NODE_TYPE(spl->expression) == PM_LOCAL_VARIABLE_TARGET_NODE) {
          pm_local_variable_target_node_t *t = (pm_local_variable_target_node_t *)spl->expression;
          char *vn = cstr(ctx, t->name);
          char *cn = make_cname(vn, false);
          int tmp = ctx->temp_counter++;
          emit(ctx, "{ %s *_rest_%d = %s_new();\n", arr_type, tmp, arr_type);
          emit(ctx, " for (mrb_int _ri_%d = %d; _ri_%d < %s(_mv_%d); _ri_%d++)\n",
             tmp, (int)n->lefts.size, tmp, len_fn, mv_id, tmp);
          emit(ctx, "  %s_push(_rest_%d, %s(_mv_%d, _ri_%d));\n", arr_type, tmp, get_fn, mv_id, tmp);
          emit(ctx, " %s = _rest_%d; }\n", cn, tmp);
          free(vn); free(cn);
        }
      }
      ctx->indent--;
      emit(ctx, "}\n");
      free(rhs_expr);
    }
    break;
  }

  /* Skip class/module/def at top level (handled in separate passes) */
  case PM_CLASS_NODE:
  case PM_MODULE_NODE:
  case PM_DEF_NODE:
    break;

  /* Handle expression-as-statement (including implicit returns, nil nodes) */
  case PM_LOCAL_VARIABLE_READ_NODE:
  case PM_INSTANCE_VARIABLE_READ_NODE:
  case PM_NIL_NODE:
  case PM_SELF_NODE:
  case PM_FLOAT_NODE:
  case PM_INTEGER_NODE:
    /* Bare expression — typically the implicit return value.
     * In void methods we discard; in non-void we could emit return. */
    break;

  default: {
    /* Try as expression statement */
    char *expr = codegen_expr(ctx, node);
    if (expr && strcmp(expr, "0") != 0 && strncmp(expr, "/* TODO", 7) != 0)
      emit(ctx, "%s;\n", expr);
    free(expr);
    break;
  }
  }
}

void codegen_stmts(codegen_ctx_t *ctx, pm_node_t *node) {
  if (!node) return;
  if (PM_NODE_TYPE(node) == PM_STATEMENTS_NODE) {
    pm_statements_node_t *s = (pm_statements_node_t *)node;
    bool saved_ir = ctx->implicit_return;
    for (size_t i = 0; i < s->body.size; i++) {
      bool is_last = (i + 1 == s->body.size);
      if (!is_last) {
        /* Not the last statement — disable implicit return */
        ctx->implicit_return = false;
      }
      else {
        ctx->implicit_return = saved_ir;
      }
      pm_node_t *stmt = s->body.nodes[i];
      /* If implicit_return and last stmt is a simple expression, emit return */
      if (is_last && ctx->implicit_return &&
        PM_NODE_TYPE(stmt) != PM_IF_NODE &&
        PM_NODE_TYPE(stmt) != PM_WHILE_NODE &&
        PM_NODE_TYPE(stmt) != PM_RETURN_NODE &&
        PM_NODE_TYPE(stmt) != PM_LOCAL_VARIABLE_WRITE_NODE &&
        PM_NODE_TYPE(stmt) != PM_CONSTANT_WRITE_NODE &&
        PM_NODE_TYPE(stmt) != PM_INSTANCE_VARIABLE_WRITE_NODE &&
        PM_NODE_TYPE(stmt) != PM_LOCAL_VARIABLE_OPERATOR_WRITE_NODE &&
        PM_NODE_TYPE(stmt) != PM_LOCAL_VARIABLE_OR_WRITE_NODE &&
        PM_NODE_TYPE(stmt) != PM_LOCAL_VARIABLE_AND_WRITE_NODE &&
        PM_NODE_TYPE(stmt) != PM_BREAK_NODE) {
        char *val = codegen_expr(ctx, stmt);
        if (val && strcmp(val, "/* nil */") != 0) {
          if (ctx->gc_scope_active)
            emit(ctx, "{ SP_GC_RESTORE(); return %s; }\n", val);
          else
            emit(ctx, "return %s;\n", val);
        }
        free(val);
        ctx->implicit_return = saved_ir;
        continue;
      }
      codegen_stmt(ctx, stmt);
    }
    ctx->implicit_return = saved_ir;
  }
  else {
    codegen_stmt(ctx, node);
  }
}
