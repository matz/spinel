# Regression: instance variables initialized via Array.new(n, FLOAT)
# must be typed as sp_FloatArray *, not the containing class's pointer.
#
# infer_ivar_init_type's CallNode/"new" branch used to unconditionally
# return "int_array" for Array.new(...) regardless of fill type. The
# resulting struct field was then typed as the class itself; assigning
# the FloatArray* into the field worked with a warning, but reads
# went through the wrong type and silently returned zero.
#
# (Use float values whose fractional part is non-zero so that
# Spinel's float-puts and CRuby's match -- "0.5" not "0.0".)

class Box
  attr_accessor :nums

  def initialize
    @nums = Array.new(3, 0.5)
  end
end

class Holder
  attr_accessor :weights

  def initialize(n)
    @weights = Array.new(n, 0.25)
    i = 0
    while i < n
      @weights[i] = i * 0.5 + 0.25
      i += 1
    end
  end
end

b = Box.new
puts b.nums[0]      # 0.5
puts b.nums[1]      # 0.5
puts b.nums[2]      # 0.5
puts b.nums.length  # 3

h = Holder.new(4)
puts h.weights[0]   # 0.25
puts h.weights[1]   # 0.75
puts h.weights[2]   # 1.25
puts h.weights[3]   # 1.75
