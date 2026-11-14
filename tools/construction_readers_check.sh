#!/usr/bin/env bash
# construction_readers_check.sh -- the static rule beside method_table_check.sh (#10866, step 2, construction): the
# definers, the passes that put methods in the tables (they inherit, include, extend, prepend, alias, specialize and
# clone them), read the tables as they build them, and those reads are no answer to a call. Each stands inside the
# check's bracket (mtab_enter / mtab_leave) and says so with a "(construction:" comment; this rule holds the list
# exact. A reader that decides what a call runs asks the plan, whatever pass it is in: it is not a definer.
# Two refusals (they ask a call's question of the tables as they stand and fill none) stand in the same bracket and
# say so with a "(refusal:" comment; they are on their own list, not the definers'.
#
#   tools/construction_readers_check.sh
#
# Fails, naming file:function, when a function below does not open and close the bracket the same number of times
# (at least once), when it returns while the bracket is open (the bracket would stay open), or when another function
# in those files carries the "(construction:" or "(refusal:" comment: the lists are the definers and the refusals,
# and no function joins them without joining its list. The rule checks these functions, not their callees: a
# callee asked inside the bracket is covered by it.
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT" || exit 2
FUNCS="
src/analyze_scope.c:inherit_members
src/analyze_scope.c:register_alias_methods
src/analyze_scope.c:register_includes
src/analyze_scope.c:register_include_attrs
src/analyze_scope.c:register_extends
src/analyze_scope.c:register_prepends
src/analyze_scope.c:resolve_inherited_aliases
src/analyze_scope.c:settle_object_builtin_aliases
src/analyze_scope.c:settle_mixin_aliases
src/analyze_scope.c:specialize_inherited_cls_new
src/analyze_scope.c:include_module_methods
src/analyze_scope.c:extend_class_with
src/analyze.c:make_yield_proc_forms
"
REFUSALS="
src/analyze.c:refuse_mixin_after_calls
src/analyze.c:refuse_reinit_mismatch
"
bad=0
for ent in $FUNCS $REFUSALS; do
  f=${ent%%:*}; fn=${ent##*:}
  res=$(awk -v fn="$fn" '
    !inb && $0 ~ "^(static )?[a-z].*[ *]" fn "[(]" && $0 !~ /;$/ { inb = 1; found = 1 }
    inb && /mtab_enter[(]c[)];/ { e++ }
    inb && e > l && /(^|[^A-Za-z0-9_])return([^A-Za-z0-9_]|$)/ && $0 !~ /^[ \t]*(\/[*]|[*])/ {
      # a return ahead of the leave on its own line leaves with the bracket open
      rp = match($0, /(^|[^A-Za-z0-9_])return([^A-Za-z0-9_]|$)/); lp = index($0, "mtab_leave(c);")
      if (lp == 0 || rp < lp) r = r " " NR }
    inb && /mtab_leave[(]c[)];/ { l++ }
    inb && /^}/ { inb = 0 }
    END { if (!found) print "function not found"; else if (e < 1 || e != l) print "bracket opened " e + 0 " and closed " l + 0 " times"
          else if (r != "") print "returns with the bracket open at line" r }' "$f")
  if [ -n "$res" ]; then echo "construction-readers-check: $f:$fn: $res"; bad=1; fi
done
# the other direction: every function carrying the comment is on the list
for f in src/analyze_scope.c src/analyze.c; do
  awk -v f="$f" '
    /^(static )?[a-z][^;]*[(]/ && !/;$/ && /[{]$/ { fn = $0; sub(/[(].*/, "", fn); n = split(fn, w, /[ *]+/); fn = w[n] }
    /[(]construction:/ { print "c " f ":" fn }
    /[(]refusal:/ { print "r " f ":" fn }' "$f"
done | sort -u | while IFS= read -r line; do
  kind=${line%% *}; ent=${line#* }
  if [ "$kind" = c ]; then
    case "$FUNCS" in *"$ent"*) ;; *) echo "construction-readers-check: $ent carries the construction bracket and is not on the definers' list"; exit 3 ;; esac
  else
    case "$REFUSALS" in *"$ent"*) ;; *) echo "construction-readers-check: $ent carries the refusal bracket and is not on the refusals' list"; exit 3 ;; esac
  fi
done || bad=1
[ $bad -eq 0 ] && echo "construction-readers-check: ok"
exit $bad
