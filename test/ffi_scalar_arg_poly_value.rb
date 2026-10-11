# A boxed value in a scalar FFI arg slot must convert by its tag, not read
# the union field the slot wants regardless of what the box holds. A boxed
# Integer in a :double slot read .v.f -- the integer's bits reinterpreted
# as a float, a denormal ~0 -- so a raylib scene passing `110` to
# DrawCircle's radius silently drew nothing. Symmetrically, a boxed Float
# in an :int slot read the float's bit pattern as an integer. The ffi gem
# converts at the boundary (Integer->float, Float->int truncation); mirror
# that. Mirrors ffi_int_arg_poly_value.rb, which pinned the extraction fix
# but only exercised INT-in-an-int-slot (the one case the union read got
# right by accident).

class Box
  attr_accessor :v
  def initialize(v)
    @v = v
  end
end

module Lib
  ffi_func :floor, [:double], :double
  ffi_func :abs, [:int], :int
end

puts Lib.floor(Box.new(110).v)   # boxed Integer in a double slot: converts
puts Lib.floor(Box.new(2.9).v)   # boxed Float in a double slot: passthrough
puts Lib.abs(Box.new(-37.9).v)   # boxed Float in an int slot: truncates
puts Lib.abs(Box.new(42).v)      # boxed Integer in an int slot: passthrough
