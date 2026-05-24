# CallOperatorWriteNode fallback path:
# explicit getter/setter methods (not attr_accessor shortcut).
#
# `obj.value += n` should lower via getter + operator + setter.

class C
  def initialize
    @v = 0xfe
  end

  def value
    @v
  end

  def value=(v)
    @v = v & 0xff
  end
end

c = C.new
puts c.value      # 254
c.value += 5
puts c.value      # 3   ((254 + 5) & 0xff)
c.value -= 4
puts c.value      # 255 ((3 - 4) & 0xff)
