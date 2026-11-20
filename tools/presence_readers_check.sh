#!/usr/bin/env bash
# presence_readers_check.sh -- the static rule beside method_table_check.sh (#10866, step 2, presence family): the
# readers that answer respond_to? and method_defined? (and what they share: the run-time-name lists, the root and IO
# reopening asks, the nil, boolean and builtin-receiver arms) read no method table themselves. What a class holds
# under a name, with which visibility, and what an undef takes away is the call plan's question (cplan_presence and
# cplan_table_presence, through presence_at; call_target for a method's own scope; cplan_method_vis); the readers only
# ask and note what it answers.
#
#   tools/presence_readers_check.sh
#
# Fails, naming file:function:line, when the body of one of the functions below names a primitive of the method
# tables (comp_method_in_class and its kin, comp_is_undeffed_in_chain, comp_is_reader and its kin, comp_*_vis*) or a
# ClassInfo array of methods (undefs, vis_names, aliases, readers, writers and their counts). The rule checks this named list,
# not the callees: a helper they call is covered by the dynamic check (method_table_check.sh, whose presence family
# is a FAIL_FAMILY and counts any other read made under a respond_to? or a method_defined?).
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT" || exit 2
FUNCS="
src/codegen_call.c:rto_exact_class_answer
src/codegen_call.c:rto_subtree_answer
src/codegen_call.c:rto_exc_by_cls
src/codegen_call.c:respond_to_user_defined
src/codegen_call.c:emit_responds_name
src/codegen_call.c:any_class_responds
src/codegen_call.c:class_value_arms
src/codegen_call.c:emit_class_cells_answer
src/codegen_call.c:class_value_responds
src/codegen_call.c:private_answer_overridden
src/codegen_call.c:bool_side_responds
src/codegen_call.c:reopen_own_live
src/codegen_call.c:rt_probe_unfounded
src/codegen_call.c:emit_io_pick_public
src/codegen_call.c:emit_io_reopen_responds
src/codegen_call.c:emit_class_value_responds_helper
src/codegen_call_class.c:emit_refl_method_defined
src/analyze_desugar.c:desugar_respond_to_probe
src/codegen_call_class.c:object_reopen_responds_name
src/codegen_call_class.c:emit_object_reopen_responds
src/codegen_call_class.c:rtr_lists_build
src/codegen_call_class.c:rtr_lists
src/codegen_call_class.c:rtr_undef
src/codegen_call_class.c:rtr_omitted
src/codegen_call_class.c:emit_respond_to_runtime_name
src/codegen_call_class.c:root_method_vis
src/codegen_call_object.c:emit_blank_slate_defined_ask
src/codegen_expr.c:defined_super_reaches
src/analyze_util.c:io_reopen_defs
"
PRIM='comp_(method_in_class|cmethod_in_class|method_in_chain|cmethod_in_chain|self_method_in_chain|method_index|main_method|included_method_index|reader_in_chain|writer_in_chain|is_undeffed_in_chain|is_reader|is_writer|is_sg_reader|is_sg_writer|name_is_sg_alias_only|method_vis_in_chain|method_vis_declared|cmethod_vis_declared|method_vis|cmethod_vis)[(]'
ARR='(->|[.])(undefs|nundefs|vis_names|vis_kinds|nvis|alias_new|alias_old|naliases|readers|nreaders|writers|nwriters|sg_readers|sg_writers|nsg_readers|nsg_writers)([^a-z_]|$)'
bad=0
for ent in $FUNCS; do
  f=${ent%%:*}; fn=${ent##*:}
  out=$(awk -v fn="$fn" '
    !inb && $0 ~ "^(static )?[a-zA-Z].*[ *]" fn "[(]" && $0 !~ /;$/ { inb = 1 }
    inb { printf "%d: %s\n", NR, $0 }
    inb && /^}/ { inb = 0; found = 1 }
    END { if (!found) print "0: function not found" }' "$f" | grep -E "$PRIM|$ARR|function not found")
  if [ -n "$out" ]; then
    printf '%s\n' "$out" | while IFS= read -r l; do echo "presence-readers-check: $f:$fn:$l"; done
    bad=1
  fi
done
[ $bad -eq 0 ] && echo "presence-readers-check: ok"
exit $bad
