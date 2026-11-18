#!/usr/bin/env bash
# siteless_readers_check.sh -- the static rule beside method_table_check.sh (#10866, step 2, siteless readers): the
# dispatchers and predicates that no call stands behind (the per-class switches of the generated C, the passes'
# surveys of "does any class define X") ask the method tables with no site, through the plan's no-site ask
# (call_target, call_answers and call_attr_target with id -1; CT_SHARED marks the asks of the readers the call sites share; a literal -1 passed as a reader's `site` parameter is the same ask for an implied name: a survey of the program's classes, a table switch or a memo, each with no call behind it), and name no table
# primitive themselves. The dynamic check lists a versioned answer of such an ask as an `unasked` table answer.
#
#   tools/siteless_readers_check.sh
#
# Two directions. Fails, naming file:function:line, when the body of one of the functions below names a primitive of
# the method tables (comp_method_in_chain and its kin, comp_reader_in_chain, comp_main_method, comp_method_index ...).
# And fails, naming file:function, when a function that is not on the list makes a no-site ask for a method scope
# (CT_SHARED, or a literal -1 passed in the position of a parameter named `site` of any function that declares one: cplan_implied_target and the readers that ask it): a new reader of that kind joins the list, so that none drops its reads from --method-table-check
# unseen. The rule checks this named list, not the callees: a helper they call is covered by the dynamic check
# (method_table_check.sh, whose `pass` family counts a read made with no node open).
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT" || exit 2
# Entries added by the S2-ASKS PR, in three groups. The program-level dispatchers, each emitted once per program for
# every class (a switch on the class id): emit_user_coerce_dispatch and emit_obj_succ_dispatch (the numeric hooks),
# hash_reader_ivar (the key hash of a class whose `hash` is a reader), emit_cls_answers_dispatch (Struct's members and
# keyword_init?), emit_exc_text_tail (the exception text hooks), emit_display_overrides
# (the object display hooks), emit_obj_to_str_bridge (implicit String conversion), and
# emit_exc_reinit_dispatch (the initializer for errors the runtime raises). No call
# stands behind an arm: the switch is the class's, so its question is the table's with no site, asked for every class
# of the program, as the others of this list are. The readers of the compiler's own synthesized helper: obj_each_elems_text
# and an_phase_procs, for every class an `__enum_to_a` call may reach (its `each` and `__enum_to_a`), whose call is a
# synthesized node that names no method of the program's. The singleton-override readers (sg_delegate_scope,
# sg_override_ret and emit_method's early-call arm): the delegate they build answers a call that runs on an object
# without the override (a dup, or the object before its def ran), after any later redefinition of the parent's method, so
# no point of the load is that call's own; the table's method is the answer until the entries of step 4.
FUNCS="
src/analyze_infer.c:class_any_cmethod_eqq
src/analyze_util.c:class_coerce_emittable
src/codegen.c:emit_cls_user_display_hooks
src/codegen.c:emit_exc_reinit_dispatch
src/codegen.c:emit_struct_default_inspect
src/codegen.c:obj_arm_calls_own_inspect1
src/codegen.c:obj_deconstruct_keys_method
src/codegen.c:pp_prints_by_rt
src/codegen.c:user_deconstruct_scope
src/compiler.c:prog_has_cls_user_display
src/analyze.c:pdn_lookup
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
src/codegen.c:emit_display_overrides
src/codegen.c:emit_obj_to_str_bridge
src/codegen.c:class_range_linear
src/codegen.c:class_succ_dispatched
src/codegen.c:class_opens_cmp
src/codegen.c:conv_bridge_callee
src/codegen_call.c:root_reopen_defines
src/compiler.c:mixin_answers_first
src/compiler.c:toplevel_mod_method
src/compiler.c:comp_poly_arm_reader_any
src/codegen.c:emit_user_coerce_dispatch
src/codegen.c:emit_obj_succ_dispatch
src/codegen.c:hash_reader_ivar
src/codegen.c:emit_cls_answers_dispatch
src/codegen.c:emit_exc_text_tail
src/codegen.c:sg_delegate_scope
src/codegen.c:emit_method
src/analyze_pass.c:sg_override_ret
src/codegen.c:obj_each_elems_text
src/analyze.c:oa_copy_name
src/codegen_stmt.c:exc_message_boxed_call
src/codegen.c:obj_to_json_method
src/codegen.c:array_user_inspect_mi
src/codegen_call_class.c:emit_refl_im_params
src/analyze_util.c:class_value_bind_call_target
src/call_plan.c:cplan_to_ary_entries
src/analyze_pass.c:bs_object_to_ary
src/codegen.c:emit_kconv_bridge
src/codegen_call.c:emit_poly_enum_for
src/analyze.c:an_phase_procs
src/analyze_desugar.c:send_split_by_owner
src/analyze_pass.c:bare_call_on_class
src/analyze_desugar.c:dsend_class_names
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
# the other direction: every function that asks the plan for a method scope with no site is on the list: by CT_SHARED, or
# structurally, by a literal -1 passed in the position of a parameter named `site` (the reader's site node) of any
# function that declares one (cplan_implied_target, the display and conversion readers, ...)
rbout=$(ruby -Eascii-8bit:ascii-8bit -e '
  files = Dir["src/*.c"].sort
  texts = (files + Dir["src/*.h"]).map { |f| [f, File.read(f)] }
  decl = {}   # name => index of its `site` parameter
  texts.each do |f, t|
    t.scan(/\b(\w+)\s*\(([^()]*\bint\s+site\b[^()]*)\)\s*[;{]/m) do |name, params|
      i = params.split(",").index { |q| q =~ /\bsite\s*\z/ }
      decl[name] = i if i
    end
  end
  call = /\b(#{decl.keys.map { |n| Regexp.escape(n) }.join("|")})\s*\(/
  head = /^[a-zA-Z_][^;=\n]*?(\w+)\(.*$/
  texts.each do |f, t|
    next unless f.end_with?(".c")
    heads = []   # [offset, function] of each definition header, in file order
    t.scan(head) do
      m = $~
      heads << [m.begin(0), m[1]] unless m[0] =~ /\A(typedef|return|else|if|for|while|switch)\b/ || m[0] =~ /;\s*\z/
    end
    t.scan(call) do
      name = $1; at = $~.end(0)
      depth = 0; arg = +""; args = []
      i = at
      while i < t.size
        ch = t[i]
        if ch == "(" then depth += 1; arg << ch
        elsif ch == ")" then break if depth == 0; depth -= 1; arg << ch
        elsif ch == "," && depth == 0 then args << arg; arg = +""
        else arg << ch end
        i += 1
      end
      args << arg
      a = args[decl[name]]
      next unless a && a.gsub(%r{/\*.*?\*/}m, "").strip == "-1"
      cur = heads.reverse.find { |o, _| o < at }
      puts "#{f}:#{cur[1]}" if cur && cur[1] != name
    end
  end
') || { echo "siteless-readers-check: the site-parameter scan failed"; exit 2; }
missing=$(
{
printf "%s\n" "$rbout"
for f in src/*.c; do
  [ "$f" = src/call_plan.c ] && continue
  awk -v f="$f" '
    /^[a-zA-Z_][^;=]*[(]/ && !/^(typedef|return|else|if|for|while|switch|#)/ && !/;[ \t]*$/ { x = $0; sub(/[(].*/, "", x); n = split(x, w, /[ *]+/); fn = w[n] }
    /CT_SHARED/ && fn != "" { print f ":" fn }' "$f"
done
} | sed -e "/^\$/d" | sort -u | while IFS= read -r ent; do
  printf '%s\n' $FUNCS | grep -Fxq -- "$ent" || echo "siteless-readers-check: $ent asks the plan with no site and is not on the readers' list"
done)
if [ -n "$missing" ]; then printf '%s\n' "$missing"; bad=1; fi
[ $bad -eq 0 ] && echo "siteless-readers-check: ok"
exit $bad
