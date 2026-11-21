#!/usr/bin/env bash
# analysis_forms_check.sh -- the analysis' compile-time forms, as the graph
# load model (the default) runs them, on the inline model's tables too.
#
#   tools/analysis_forms_check.sh [SPINEL]
#
# The inline load model expands a class body's loop over a constant table
# with its text pass (sp_const_loop.c) before the parse; the analysis runs
# the same code with its compile-time evaluator (analyze_ct.c), on graph
# tables or under the test-only switch SPINEL_TABLE_LOOPS=analysis. The
# corpus runs the graph model; this checks the analysis' side under the
# switch as well, where the text pass would otherwise answer first:
#
# - test/analysis-forms/*.rb but the class macros (below), the programs the
#   analysis answers (some the text pass refuses), and
#   test/class_table_loop_*.rb, compiled under the switch and run plain,
#   under --int-overflow=promote and under SPINEL_GC_STRESS=1, each against
#   its .expected;
# - test/reject/table_loop_*.rb, which must be refused under the switch
#   too, unless test/analysis-forms/ holds a program of the same name: one
#   the text pass refuses and the analysis answers.
#
# A class body's call to a macro its class defines is expanded by the text
# pass too (sp_macro.c); the analysis runs the macro at the call's point
# (ct_macro_form), on the graph model or under SPINEL_CLASS_MACROS=analysis,
# which turns the text pass off. Under that switch:
#
# - test/analysis-forms/class_macro_*.rb and the corpus programs the form
#   answers (MACRO_FORMS) are run as above, and compiled plain under
#   --load-model=graph too, the switch unset;
# - every other test/class_macro_*.rb and test/define_method_*.rb is refused,
#   or answers its .expected and exits 0;
# - test/reject/*macro*.rb stays refused, unless test/analysis-forms/
#   holds it as class_<name>: one the text pass refuses and the form
#   answers;
# - test/analysis-forms/declines/*.rb, one program per reason the form
#   leaves a call as written, is refused, under the switch and under
#   --load-model=graph (each .expected is CRuby's answer, for the day the
#   form takes it).
#
# Exit status: 0 all pass, 1 a failure.
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT" || exit 1
SP=${1:-$ROOT/bin/spinel}
TIMEOUT=$ROOT/build/spinel-timeout
[ -x "$TIMEOUT" ] && TO="$TIMEOUT 60" || TO=
export SPINEL_TABLE_LOOPS=analysis
tmp=$(mktemp -d /tmp/spinel-aforms.XXXXXX) || exit 1
trap 'rm -rf "$tmp"' EXIT
ok=1; n=0

MACRO_FORMS="test/class_macro_through_superclass.rb test/define_method_computed_name_implicit_rescue.rb
  test/class_macro_called_from_another_macro.rb test/class_macro_keyword_param_attr_reader.rb
  test/class_method_attr_macro.rb test/class_method_define_method_macro.rb test/class_macro_self_name.rb
  test/class_macro_name_of_an_attr_reader.rb test/class_body_call_reaches_generated_class_method.rb"

# Compile $1 plain and under promote (and, given a second argument, plain
# under --load-model=graph without SPINEL_CLASS_MACROS), run each, and run
# the plain build under GC stress, against its .expected
run_form() {
  t=$1; b=$(basename "$t" .rb); n=$((n + 1)); modes="plain promote${2:+ graph}"
  for mode in $modes; do
    fl=; ev=; [ $mode = promote ] && fl=--int-overflow=promote
    [ $mode = graph ] && { fl=--load-model=graph; ev="env -u SPINEL_CLASS_MACROS"; }
    if ! $ev "$SP" $fl "$t" -o "$tmp/$b.$mode" >"$tmp/$b.cc" 2>&1; then
      echo "analysis-forms-test: FAIL ($t: $mode compile)"; sed -n 1,5p "$tmp/$b.cc"; ok=0; continue
    fi
    $TO "$tmp/$b.$mode" >"$tmp/$b.out" 2>&1; rc=$?
    [ $rc -eq 0 ] || { echo "analysis-forms-test: FAIL ($t: $mode run exits $rc)"; ok=0; }
    cmp -s "$tmp/$b.out" "$t.expected" ||
      { echo "analysis-forms-test: FAIL ($t: $mode output)"; diff -u "$t.expected" "$tmp/$b.out" | head -10; ok=0; }
    if [ $mode = plain ]; then
      SPINEL_GC_STRESS=1 $TO "$tmp/$b.$mode" >"$tmp/$b.out" 2>&1; rc=$?
      [ $rc -eq 0 ] || { echo "analysis-forms-test: FAIL ($t: run under SPINEL_GC_STRESS=1 exits $rc)"; ok=0; }
      cmp -s "$tmp/$b.out" "$t.expected" ||
        { echo "analysis-forms-test: FAIL ($t: output under SPINEL_GC_STRESS=1)"; diff -u "$t.expected" "$tmp/$b.out" | head -10; ok=0; }
    fi
    rm -f "$tmp/$b.$mode"
  done
}

for t in test/analysis-forms/*.rb test/class_table_loop_*.rb; do
  case $t in */class_macro_*) continue ;; esac   # the text pass leaves some to the form: below
  [ -f "$t" ] && run_form "$t"
done

for t in test/reject/table_loop_*.rb; do
  [ -f "$t" ] || continue
  [ -f "test/analysis-forms/$(basename "$t")" ] && continue
  n=$((n + 1))
  if "$SP" "$t" -c --no-line-map -o "$tmp/r.c" >"$tmp/r.out" 2>&1; then
    echo "analysis-forms-test: FAIL ($t: compiled under SPINEL_TABLE_LOOPS=analysis)"; ok=0
  fi
done

export SPINEL_CLASS_MACROS=analysis
for t in test/analysis-forms/class_macro_*.rb $MACRO_FORMS; do
  [ -f "$t" ] && run_form "$t" graph
done

for t in test/class_macro_*.rb test/define_method_*.rb; do
  [ -f "$t" ] || continue
  case " $(echo $MACRO_FORMS) " in *" $t "*) continue ;; esac
  n=$((n + 1))
  "$SP" "$t" -o "$tmp/m.bin" >"$tmp/m.cc" 2>&1 || continue
  $TO "$tmp/m.bin" >"$tmp/m.out" 2>&1; rc=$?
  [ $rc -eq 0 ] || { echo "analysis-forms-test: FAIL ($t: compiled under SPINEL_CLASS_MACROS=analysis, run exits $rc)"; ok=0; }
  cmp -s "$tmp/m.out" "$t.expected" ||
    { echo "analysis-forms-test: FAIL ($t: compiled under SPINEL_CLASS_MACROS=analysis, wrong output)"; diff -u "$t.expected" "$tmp/m.out" | head -10; ok=0; }
  rm -f "$tmp/m.bin"
done

for t in test/reject/*macro*.rb; do
  [ -f "$t" ] || continue
  [ -f "test/analysis-forms/class_$(basename "$t")" ] && continue
  n=$((n + 1))
  if "$SP" "$t" -c --no-line-map -o "$tmp/r.c" >"$tmp/r.out" 2>&1; then
    echo "analysis-forms-test: FAIL ($t: compiled under SPINEL_CLASS_MACROS=analysis)"; ok=0
  fi
done

for t in test/analysis-forms/declines/*.rb; do
  [ -f "$t" ] || continue
  n=$((n + 1))
  for fl in "" --load-model=graph; do
    ev=; [ -n "$fl" ] && ev="env -u SPINEL_CLASS_MACROS"
    if $ev "$SP" $fl "$t" -c --no-line-map -o "$tmp/r.c" >"$tmp/r.out" 2>&1; then
      echo "analysis-forms-test: FAIL ($t: the form took it, ${fl:-under SPINEL_CLASS_MACROS=analysis})"; ok=0
    fi
  done
done

if [ $ok = 1 ]; then echo "analysis-forms-test: pass ($n programs)"; exit 0; fi
exit 1
