# `a, b = float_array_ptr_array[i]` destructures a typed
# float_array RHS. Without the typed-array dispatch in
# compile_multi_write, the temp was declared `sp_IntArray *`
# while compile_expr emitted a `(sp_FloatArray *)` cast — the
# mismatch tripped -Wincompatible-pointer-types and the
# subsequent `sp_IntArray_get` read float bytes through the int
# accessor, garbling the destructured values.

TBL = [[-0.12, 0.40], [0.00, 0.68], [0.31, 1.00], [0.72, 1.00]]

a, b = TBL[0]
puts a
puts b

c, d = TBL[3]
puts c
puts d

# Same shape with str_array elements — `s, t = str_arr_ptr_arr[i]`
# falls through to the same default. Pre-fix the temp typed as
# `sp_IntArray *` and the rebind to a string emitted
# `sp_IntArray * = const char *`.
LABELS = [["lo", "low"], ["hi", "high"], ["mid", "middle"]]

s, t = LABELS[1]
puts s
puts t
