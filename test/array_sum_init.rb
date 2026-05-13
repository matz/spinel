# Array#sum's init argument was silently dropped on the IntArray /
# FloatArray dispatch paths -- spinel emitted sp_IntArray_sum(rc)
# (no init parameter) and the block-form accumulator in
# compile_array_sum_block was hardcoded to `mrb_int t = 0;`.
# Pre-fix:
#   [1,2,3].sum(10)             # => 6   (CRuby: 16)
#   [].sum(7)                   # => 0   (CRuby: 7)
#   [1,2,3].sum(10) { |x| x*2 } # => 12  (CRuby: 22)
#
# Fix: sp_IntArray_sum / sp_FloatArray_sum take an init parameter
# and seed the accumulator with it; codegen forwards
# compile_arg0(nid) at every sum dispatch. compile_array_sum_block
# initialises tmp_sum from compile_arg0(nid).

# IntArray, no block
puts [1, 2, 3].sum(10)            # 16
puts [1, 2, 3].sum                # 6   (no-init regression check)
puts [].sum(7)                    # 7   (empty + init survives)
puts [].sum                       # 0   (empty default still 0)
puts [1, 2].sum(-5)               # -2  (negative init)

# IntArray, with block
puts [1, 2, 3].sum(10) { |x| x * 2 }   # 22
puts [1, 2, 3].sum { |x| x * 2 }       # 12  (block, no init)
puts [].sum(7) { |x| x * 2 }           # 7   (empty + init w/ block)

# FloatArray, no block
puts [1.5, 2.5].sum(0.5)          # 4.5
puts [1.5, 2.5].sum               # 4.0 (no-init regression check)
puts [1.5, 2.5].sum(1)            # 5.0 (int init implicitly widens)
puts [1.5, 2.5].sum(-0.5)         # 3.5 (negative float init)

# Poly init: a heterogeneous-array element resolves to a poly local.
# compile_arg0_as_int / compile_arg0_as_float unbox via `.v.i` / `.v.f`;
# without them gcc rejects passing sp_RbVal to mrb_int / mrb_float.
poly_arr = [10, "x"]
init_p = poly_arr[0]
puts [1, 2].sum(init_p)           # 13 (poly init unboxed as mrb_int)

fpoly_arr = [1.5, "x"]
finit_p = fpoly_arr[0]
puts [1.0, 2.0].sum(finit_p)      # 4.5 (poly init unboxed as mrb_float)

# Tag-mixed: poly is INT-tagged but the FloatArray expects mrb_float.
# sp_poly_to_f dispatches on the tag so the int value is coerced to
# float rather than reinterpreted bit-for-bit from `.v.f`.
ipoly_for_float = [1, "x"][0]
puts [1.0, 2.0].sum(ipoly_for_float)   # 4.0 (poly INT->float via sp_poly_to_f)

# FloatArray + block + float init: compile_array_sum_block now picks
# an mrb_float accumulator and compile_arg0_as_float when recv_type is
# float_array, so the 0.5 seed and the float block result are no
# longer truncated. Use distinct block-param names (`fx` / `fy`) so
# they don't get widened by the earlier int blocks' `|x|` -- spinel
# hoists block params to a shared function-scope local, an orthogonal
# limitation outside this PR's scope.
puts [1.5, 2.5].sum(0.5) { |fx| fx }      # 4.5
puts [1.0, 2.0].sum { |fy| fy * 1.5 }     # 4.5

puts "done"
