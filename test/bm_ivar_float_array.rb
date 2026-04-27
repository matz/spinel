# Regression: instance variables initialized via Array.new(n, FILL) must be
# typed by inspecting the fill argument, not always returned as "int_array".
#
# infer_ivar_init_type's CallNode/"new" branch used to unconditionally return
# "int_array" for Array.new(...). That mistyped class fields as the
# containing class's pointer; pointer-type fills additionally lost their GC
# scan function, which would let live elements be swept.
#
# We now check the fill type and use the appropriate typed array container
# (FloatArray for float fills, StrArray for string, sym_array (IntArray
# internally) for symbol, PtrArray for object/pointer fills).
#
# Use float values whose fractional part is non-zero so Spinel's float-puts
# matches CRuby's.

class Box
  attr_accessor :nums
  def initialize
    @nums = Array.new(3, 0.5)
  end
end

class SymHolder
  attr_accessor :tags
  def initialize
    @tags = Array.new(2, :alpha)
  end
end

class Marker
  attr_accessor :id
  def initialize(id)
    @id = id
  end
end

class ObjHolder
  attr_accessor :marks
  def initialize
    @marks = Array.new(3, Marker.new(42))
  end
end

b = Box.new
puts b.nums[0]      # 0.5
puts b.nums[1]      # 0.5
puts b.nums[2]      # 0.5
puts b.nums.length  # 3

s = SymHolder.new
puts s.tags[0]      # alpha
puts s.tags[1]      # alpha
puts s.tags.length  # 2

m = ObjHolder.new
puts m.marks[0].id  # 42
puts m.marks[1].id  # 42  (Array.new(n, obj) shares the same obj)
puts m.marks[2].id  # 42
puts m.marks.length # 3
