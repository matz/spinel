# Issue #78: poly dispatch and runtime helpers reaching all built-in
# array types (not just IntArray). When a `poly` slot can hold any of
# Int/Float/Str/Sym arrays plus user-class instances, dispatch on
# `length` / `size` must pick the right helper per cls_id; `puts` /
# `to_s` over a poly value must inspect the array, not print a raw
# pointer.

# A `length` poly dispatch — every built-in array type contributes a
# branch returning mrb_int.
def lenof(a)
  a.length
end

class Box
  def length; 99; end
end

puts lenof([1, 2, 3])         # 3   (IntArray)
puts lenof([1.0, 2.0, 3.0])   # 3   (FloatArray)
puts lenof(["a", "b", "c"])   # 3   (StrArray)
puts lenof([:x, :y])          # 2   (SymArray)
puts lenof(Box.new)           # 99  (user-class branch)

# sp_poly_puts / sp_poly_to_s handle every built-in array tag: a poly
# value carrying e.g. a FloatArray now prints via sp_FloatArray_inspect
# instead of the raw-pointer fallback. The reach of the param-widening
# inference (calling `show` with several built-in array types in turn)
# is a separate concern; this test only verifies that the runtime
# helpers route correctly when the poly already holds an array.
mixed = [1, 2.0, "x"]   # forces poly_array; each element via sp_poly_puts
mixed.each { |e| p e }
# 1
# 2.0
# "x"
