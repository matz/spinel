# Built-in pointer types (IntArray, etc.) need to be boxable into a
# poly value so they can flow through a parameter typed as poly.
# Before this fix, `box_expr_to_poly` fell through to `sp_box_int(v)`
# for anything that wasn't a recognized scalar / `obj_<Class>` —
# passing a `sp_IntArray *` to `sp_box_int` (which wants `mrb_int`)
# is a C type error, so the program simply didn't compile.

def kind_of(a)
  a.nil? ? "nil" : "something"
end

puts kind_of(Object.new)
puts kind_of([1, 2, 3])
