#!/usr/bin/env bash
# lists_readers_check.sh -- the static rule beside method_table_check.sh (#10866, step 2, lists family): the method list
# builders and arms read no method table and no ClassInfo method array themselves. What a list holds, with which
# visibility, and what an undef takes away is the call plan's question (cplan_list_candidates, cplan_module_methods,
# cplan_table_presence); the builders and arms only note what it answers.
#
#   tools/lists_readers_check.sh
#
# Fails, naming file:function:line, when the body of one of the functions below names a primitive of the method
# tables (comp_method_in_class and its kin, comp_is_undeffed_in_chain, comp_*_vis*) or a ClassInfo array of methods
# (undefs, vis_names, aliases, readers, writers and their counts), a scan or index of the scope table (c->nscopes,
# scopes[], a pointer taken into it to walk it) or a scope lookup by name (comp_resolve_member,
# an_scopes_named_first): scans and lookups, not the facts of one scope already in hand (a Scope pointer's fields,
# or its index by pointer difference, are no walk of the table). The rule checks this named list
# of builders, not their callees: a helper they call is covered by the dynamic check (method_table_check.sh, whose
# lists family counts a list's read made outside the plan's frame).
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT" || exit 2
FUNCS="
src/codegen_call.c:refl_cand_note
src/codegen_call.c:refl_own_instance_methods
src/codegen_call.c:refl_object_methods
src/codegen_call.c:refl_class_instance_methods
src/codegen_call.c:refl_module_instance_methods
src/codegen_call.c:refl_module_own_methods
src/codegen_call.c:refl_cmethod_cand_note
src/codegen_call.c:refl_class_singleton_methods
src/codegen_call.c:refl_emit_sym_array
src/codegen_call.c:refl_object_own_methods
src/codegen_call.c:emit_object_methods_list
src/codegen_call.c:emit_poly_object_methods_reflection
src/codegen_call.c:emit_object_methods_reflection
src/codegen_call.c:refl_value_methods
src/codegen_call.c:emit_value_methods_list
src/codegen_call.c:emit_value_methods_helpers
src/codegen_call.c:emit_value_methods_reflection
src/codegen_call_class.c:emit_class_instance_methods_all
src/codegen_call_class.c:emit_class_value_singleton_methods
src/codegen_call_class.c:emit_class_own_instance_methods
"
PRIM='comp_(method_in_class|cmethod_in_class|method_in_chain|cmethod_in_chain|self_method_in_chain|method_index|main_method|included_method_index|reader_in_chain|writer_in_chain|is_undeffed_in_chain|is_reader|is_writer|is_sg_reader|is_sg_writer|method_vis_in_chain|method_vis_declared|cmethod_vis_declared|method_vis|cmethod_vis)[(]'
ARR='(->|[.])(undefs|nundefs|vis_names|vis_kinds|nvis|alias_new|alias_old|naliases|readers|nreaders|writers|nwriters|sg_readers|sg_writers|nsg_readers|nsg_writers)([^a-z_]|$)|c->nscopes|scopes[[]|[=&] *c->scopes *([+;)]|$)|c->scopes *[+]|comp_resolve_member|an_scopes_named_first'
bad=0
for ent in $FUNCS; do
  f=${ent%%:*}; fn=${ent##*:}
  out=$(awk -v fn="$fn" '
    !inb && $0 ~ "^[a-z].*[ *]" fn "[(]" && $0 !~ /;$/ { inb = 1 }
    inb { printf "%d: %s\n", NR, $0 }
    inb && /^}/ { inb = 0; found = 1 }
    END { if (!found) print "0: function not found" }' "$f" | grep -E "$PRIM|$ARR|function not found")
  if [ -n "$out" ]; then
    printf '%s\n' "$out" | while IFS= read -r l; do echo "lists-readers-check: $f:$fn:$l"; done
    bad=1
  fi
done
[ $bad -eq 0 ] && echo "lists-readers-check: ok"
exit $bad
