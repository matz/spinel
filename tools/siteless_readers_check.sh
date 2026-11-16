#!/usr/bin/env bash
# siteless_readers_check.sh -- the static rule beside method_table_check.sh (#10866, step 2, siteless readers): the
# dispatchers and predicates that no call stands behind (the per-class switches of the generated C, the passes'
# surveys of "does any class define X") ask the method tables with no site, through the plan's no-site ask
# (call_target, call_answers and call_attr_target with id -1; CT_SHARED marks the asks of the readers the call sites share), and name no table
# primitive themselves. The dynamic check lists a versioned answer of such an ask as an `unasked` table answer.
#
#   tools/siteless_readers_check.sh
#
# Two directions. Fails, naming file:function:line, when the body of one of the functions below names a primitive of
# the method tables (comp_method_in_chain and its kin, comp_reader_in_chain, comp_main_method, comp_method_index ...).
# And fails, naming file:function, when a function that is not on the list makes a no-site ask for a method scope
# (CT_SHARED): a new reader of that kind joins the list, so that none drops its reads from --method-table-check
# unseen. The rule checks this named list, not the callees: a helper they call is covered by the dynamic check
# (method_table_check.sh, whose `pass` family counts a read made with no node open).
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT" || exit 2
FUNCS="
src/codegen_call.c:cls_side_program_answers
src/analyze_infer.c:class_any_cmethod_eqq
src/analyze_util.c:class_has_coerce_shape
src/analyze_util.c:class_coerce_emittable
src/analyze.c:pdn_lookup
src/analyze.c:ur_iter_overridden
src/analyze.c:synth_enum_to_a
src/analyze.c:synth_struct_each
src/analyze.c:synth_to_enum_generators
src/analyze.c:bsu_own_method
src/analyze.c:desugar_builtin_method_obj
src/analyze_desugar.c:desugar_array_at
src/analyze_desugar.c:desugar_array_first_last
src/analyze_desugar.c:desugar_enum_walk_calls
src/analyze_desugar.c:desugar_unpack_block
src/analyze_desugar.c:desugar_interp_reopened_to_s
src/analyze_desugar.c:desugar_interp_elided_to_s
src/analyze_pass.c:widen_shared_cmp_params
src/analyze_pass.c:boxed_store_hash_subclass_evidence
src/analyze_pass.c:propagate_prep_params
src/codegen.c:class_has_user_binop
src/codegen.c:class_gen_valeq
src/codegen.c:emit_obj_cmp_dispatch
src/codegen.c:prepare_obj_cmp_dispatch
src/codegen.c:emit_struct_display_decls
src/codegen.c:exc_text_method
src/codegen.c:range_rat_walk_own_def
src/codegen.c:emit_poly_iter_obj_normalize
src/codegen.c:user_aset_scope
src/codegen.c:obj_to_ary_method
src/codegen.c:obj_to_a_method
src/codegen.c:class_is_hashkey
src/codegen.c:class_to_io_dispatched
src/codegen.c:emit_user_to_io_dispatch
src/codegen.c:class_is_hashonly_key
src/codegen.c:class_is_valuekey
src/codegen.c:emit_user_op_dispatch
src/codegen.c:program_has_user_eql
src/codegen.c:emit_obj_valeq_dispatch
src/codegen.c:emit_obj_hashkey_dispatch
src/codegen.c:emit_obj_inspect_dispatch
src/codegen.c:class_range_linear
src/codegen.c:class_succ_dispatched
src/codegen.c:class_opens_cmp
src/codegen.c:conv_bridge_callee
src/codegen.c:display_method_scope
src/codegen.c:comp_deconstruct_method
src/codegen.c:comp_deconstruct_keys_method
src/codegen_call.c:root_reopen_defines
src/compiler.c:mixin_answers_first
src/compiler.c:cls_user_display_mi
src/compiler.c:toplevel_mod_method
src/codegen_call_exception.c:emit_call_exception_arms
src/compiler.c:comp_poly_arm_reader_any
"
PRIM='comp_(method_in_class|cmethod_in_class|method_in_chain|cmethod_in_chain|self_method_in_chain|method_index|main_method|included_method_index|reader_in_chain|writer_in_chain|is_undeffed_in_chain|method_vis_in_chain)[[:space:]]*([(]|$)'
bad=0
for ent in $FUNCS; do
  f=${ent%%:*}; fn=${ent##*:}
  [ -f "$f" ] || { echo "siteless-readers-check: $f: no such file"; bad=1; continue; }
  out=$(awk -v fn="$fn" '
    !inb && $0 ~ "^(static )?[a-z].*[ *]" fn "[(]" && $0 !~ /;$/ { inb = 1 }
    inb { printf "%d: %s\n", NR, $0 }
    inb && /^}/ { inb = 0; found = 1 }
    END { if (!found) print "0: function not found" }' "$f" | grep -E "$PRIM|function not found")
  if [ -n "$out" ]; then
    printf '%s\n' "$out" | while IFS= read -r l; do echo "siteless-readers-check: $f:$fn:$l"; done
    bad=1
  fi
done
# the other direction: every function that asks the plan for a method scope with no site is on the list
for f in src/*.c; do
  case "$f" in src/call_plan.c) continue ;; esac
  awk -v f="$f" '
    /^[a-zA-Z_][^;=]*[(]/ && !/^(typedef|return|else|if|for|while|switch|#)/ && !/;[ \t]*$/ { x = $0; sub(/[(].*/, "", x); n = split(x, w, /[ *]+/); fn = w[n] }
    /CT_SHARED/ && fn != "" { print f ":" fn }' "$f"
done | sort -u | while IFS= read -r ent; do
  printf '%s\n' $FUNCS | grep -Fxq -- "$ent" || { echo "siteless-readers-check: $ent asks the plan with no site (CT_SHARED) and is not on the readers' list"; exit 3; }
done || bad=1
[ $bad -eq 0 ] && echo "siteless-readers-check: ok"
exit $bad
