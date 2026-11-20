#!/usr/bin/env bash
# repr_diff.sh -- compare the representation two compilers choose for each
# slot of the same programs (#7501).
#
#   tools/repr_diff.sh REF_SPINEL NEW_SPINEL PROGS...
#   tools/repr_diff.sh REF_SPINEL NEW_SPINEL --pairs-dir DIR
#
# A slot's representation decides what every use of it costs: a String
# slot that becomes a String buffer (strbuf, sp_String *: the shared handle
# or the buffer a loop builds in) is copied at each call that only reads it
# (#7482), a slot that becomes boxed dispatches out of line, a slot that
# leaves a by-value layout goes through the heap. None of it changes a
# program's output, so no test sees it. This dumps each program's slots
# with both compilers (`spinel --dump-repr`: locals, parameters and the
# value of each method, ivars per class, globals, constants), pairs them by
# name and summarises what moved:
#
#   N slots became String buffers (strbuf), M became boxed, K left a
#   by-value layout, J other changes
#
# then lists the programs and slots. A compiler older than --dump-repr is
# compared by the slot declarations in its generated C instead (-S
# --no-line-map: struct fields, globals, constants, parameters, locals and
# return types, with their C types); both sides then use the C, so the two
# are comparable. REPR_DIFF_MODE=c forces that mode.
#
# Exit status: 0, or 2 on a usage or infrastructure error. The summary is
# a report, not a verdict.
set -u
[ $# -ge 3 ] || { sed -n '3,4p' "$0" | sed 's/^# \{0,1\}//' >&2; exit 2; }
REF=$1 NEW=$2; shift 2
[ -x "$REF" ] && [ -x "$NEW" ] || { echo "repr_diff: $REF and $NEW must be spinel binaries" >&2; exit 2; }
PAIRS_DIR=
if [ "$1" = --pairs-dir ]; then
  [ $# -eq 2 ] && [ -d "$2" ] || { echo 'repr_diff: --pairs-dir needs a directory' >&2; exit 2; }
  PAIRS_DIR=$2
  shopt -s nullglob
  set -- "$PAIRS_DIR"/*.name
  shopt -u nullglob
  [ $# -gt 0 ] || { echo 'repr_diff: no pairs in the directory' >&2; exit 2; }
fi
JOBS=${REPR_DIFF_JOBS:-2}
T=$(mktemp -d "${TMPDIR:-/tmp}/spinel-repr-diff.XXXXXX") || exit 2
trap 'rm -rf "$T"' EXIT

MODE=${REPR_DIFF_MODE:-}
if [ -z "$MODE" ]; then
  printf 'x = 1\np x\n' > "$T/probe.rb"
  MODE=repr
  for sp in "$REF" "$NEW"; do "$sp" --dump-repr "$T/probe.rb" > /dev/null 2>&1 || MODE=c; done
fi

# The slots a compiler's C declares, one `<where>: c=<C type>` line each.
DECLS_AWK='
function norm(t) { gsub(/[ \t]+/, " ", t); sub(/^ /, "", t); sub(/ $/, "", t); gsub(/ \*/, " *", t); return t }
/^struct sp_[A-Za-z0-9_]+_s \{$/ { cls = $2; sub(/^sp_/, "", cls); sub(/_s$/, "", cls); next }
cls != "" && /^\}( SP_MAY_ALIAS)?;/ { cls = ""; next }
cls != "" && match($0, /iv_[A-Za-z0-9_]+;$/) {
  t = substr($0, 1, RSTART - 1); nm = substr($0, RSTART + 3, RLENGTH - 4)
  print "ivar " cls " @" nm ": c=" norm(t); next
}
/^static [^(]*(gv|cst|civ)_[A-Za-z0-9_]+ = / {
  d = $0; sub(/ = .*/, "", d); sub(/^static /, "", d)
  match(d, /(gv|cst|civ)_[A-Za-z0-9_]+$/)
  t = substr(d, 1, RSTART - 1); nm = substr(d, RSTART)
  kind = nm ~ /^gv_/ ? "gvar $" : nm ~ /^cst_/ ? "const " : "class-ivar "
  sub(/^(gv|cst|civ)_/, "", nm)
  print kind nm ": c=" norm(t); next
}
/^[A-Za-z_][^;=]*\) ?\{$/ && /(^|[ *])(sp_[A-Za-z0-9_]+|_sp_main_body)\(/ {
  d = $0; sub(/\) ?\{$/, "", d); gsub(/__attribute__\(\([^)]*\)\) ?/, "", d)
  p = index(d, "("); head = substr(d, 1, p - 1); params = substr(d, p + 1)
  match(head, /[A-Za-z0-9_]+$/); fn = substr(head, RSTART); rt = substr(head, 1, RSTART - 1)
  gsub(/(static|inline|SP_[A-Z_]+) /, "", rt)
  user = fn == "_sp_main_body"
  np = split(params, pa, ",")
  for (k = 1; k <= np; k++) {
    if (pa[k] ~ /[ *]self$/) user = 1
    if (match(pa[k], /(lv|_cell)_[A-Za-z0-9_]+$/)) {
      user = 1
      nm = substr(pa[k], RSTART); sub(/^(lv|_cell)_/, "", nm)
      print "param " fn " " nm ": c=" norm(substr(pa[k], 1, RSTART - 1))
    }
  }
  infn = fn; next
}
/^\}/ { if (infn != "" && user) print "ret " infn ": c=" norm(rt); infn = ""; next }
infn != "" && /^    [A-Za-z_][^=(;]* lv_[A-Za-z0-9_]+( = [^;]*)?;$/ {
  d = $0; sub(/ = .*/, "", d); sub(/;$/, "", d)
  match(d, /lv_[A-Za-z0-9_]+$/)
  t = substr(d, 1, RSTART - 1); sub(/^ +(volatile )?/, "", t)
  print "local " infn " " substr(d, RSTART + 3) ": c=" norm(t); user = 1
}'
export MODE DECLS_AWK PAIRS_DIR

# one dump per program and side, each job keyed by its input position so
# no two paths share a file; a block parameter's numbered suffix
# (`i__bp49`) follows node ids, so it is dropped
i=0
for p in "$@"; do i=$((i + 1)); printf '%06d:%s\0' "$i" "$p"; done | xargs -0 -P "$JOBS" -n 1 bash -c '
  k=${4%%:*}; p=${4#*:}
  pair=
  if [ -n "$PAIRS_DIR" ]; then
    pair=${p%.name}
    p=$(cat "$p") || exit 2
  fi
  for side in ref new; do
    sp=$1; [ $side = new ] && sp=$2
    if [ "$MODE" = repr ]; then "$sp" --dump-repr "$p" > "$3/$k.$side.raw" 2>/dev/null
    elif [ -n "$pair" ] && [ -r "$pair.$side.c" ] && [ -z "${SPINEL_DECISIONS_LOG+x}${SPINEL_EMIT_TRACE+x}" ]; then
      awk "$DECLS_AWK" "$pair.$side.c" > "$3/$k.$side.raw"
    else "$sp" -S --no-line-map "$p" 2>/dev/null > "$3/$k.$side.c" && awk "$DECLS_AWK" "$3/$k.$side.c" > "$3/$k.$side.raw"; fi
    rc=$?
    rm -f "$3/$k.$side.c"
    [ $rc -eq 0 ] || { echo "$p" > "$3/$k.fail"; continue; }
    sed -E "s/__bp[A-Za-z0-9_]+/__bp/g" "$3/$k.$side.raw" | LC_ALL=C sort > "$3/$k.$side"
  done
  printf "%s\n" "$p" > "$3/$k.name"' _ "$REF" "$NEW" "$T" || { echo "repr_diff: dumping the programs failed" >&2; exit 2; }

# Pair the slots by name (a repeated name pairs in order) and classify each
# change: a slot that became a String buffer, became boxed, or left a
# by-value layout, else another change. A slot only one side has is another
# change too. A String buffer is the strbuf kind, shared handle or not:
# both are an sp_String * whose read-only uses copy, as the C mode sees.
for f in "$T"/*.name; do
  [ -f "$f" ] || continue
  b=${f%.name}; p=$(cat "$f")
  if [ -f "$b.fail" ]; then printf 'S\t%s\n' "$p"; continue; fi
  awk -v prog="$p" -v mode="$MODE" -v OFS='\t' '
    function key(l) { return substr(l, 1, index(l, ": ") - 1) }
    function val(l) { return substr(l, index(l, ": ") + 2) }
    function kind(v) { split(v, w, " "); return w[1] }
    function strbuf(v) { return mode == "repr" ? kind(v) == "strbuf" : v ~ /sp_String \*/ }
    function boxed(v) { return mode == "repr" ? kind(v) == "boxed" : v == "c=sp_RbVal" }
    function byval(v) { return mode == "repr" ? kind(v) ~ /^(scalar|sentinel|struct|vobj)$/ : (v !~ /\*/ && v != "c=sp_RbVal") }
    {
      # the side is the file, not its first line: a dump can be empty
      side = FILENAME == ARGV[1] ? 1 : 2
      k = key($0); n = ++cnt[side, k]
      if (side == 1) { old[k, n] = val($0) } else { new[k, n] = val($0) }
      keys[k] = 1
    }
    END {
      for (k in keys) {
        m = cnt[1, k] > cnt[2, k] ? cnt[1, k] : cnt[2, k]
        for (n = 1; n <= m; n++) {
          o = ((k, n) in old) ? old[k, n] : "-"; v = ((k, n) in new) ? new[k, n] : "-"
          if (o == v) continue
          c = "other"
          if (o != "-" && v != "-") {
            if (strbuf(v) && !strbuf(o)) c = "strbuf"
            else if (boxed(v) && !boxed(o)) c = "boxed"
            else if (byval(o) && !byval(v)) c = "unvalue"
          }
          print "D", c, prog, k, o, v
        }
      }
    }' "$b.ref" "$b.new"
done | LC_ALL=C sort -t "$(printf '\t')" -k3,3 -k4,4 | awk -F'\t' -v mode="$MODE" -v total=$# '
  $1 == "S" { s[++ns] = $2; next }
  {
    n[$2]++; if (!($3 in progs)) { progs[$3] = 1; np++ }
    line[$2] = line[$2] sprintf("  %s: %s: %s -> %s\n", $3, $4, $5, $6)
  }
  END {
    printf "repr_diff (%s): %d slots became String buffers (strbuf), %d became boxed, %d left a by-value layout, %d other changes, in %d of %d programs%s\n",
      (mode == "repr" ? "--dump-repr" : "C slot declarations"), n["strbuf"], n["boxed"], n["unvalue"], n["other"], np, total,
      (ns ? sprintf(" (%d did not compile on a side)", ns) : "")
    split("strbuf boxed unvalue other", order, " ")
    title["strbuf"] = "became String buffers (strbuf):"; title["boxed"] = "became boxed:"
    title["unvalue"] = "left a by-value layout:"; title["other"] = "other changes:"
    for (i = 1; i <= 4; i++) if (n[order[i]]) { print title[order[i]]; printf "%s", line[order[i]] }
    for (i = 1; i <= ns; i++) print "  did not compile: " s[i]
  }'
