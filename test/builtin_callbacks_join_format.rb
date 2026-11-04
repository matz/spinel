# Array#* with a String argument joins the elements, and String#% with an
# Array or a Hash argument formats the values it holds: both call the
# program's own to_s on those elements. The callback table did not list
# to_s for these shapes, so the method was not kept and the default
# #<K:0x...> text was printed. Each case has its own class, so that no
# other call keeps its to_s.
class J
  def initialize(v) = @v = v
  def to_s = "J#{@v}"
end
p [J.new(1), J.new(2)] * ","

class F
  def initialize(v) = @v = v
  def to_s = "F#{@v}"
end
p "%s" % [F.new(1)]

class G
  def initialize(v) = @v = v
  def to_s = "G#{@v}"
end
p "%-4s|%s" % [G.new(1), G.new(2)]

class N
  def initialize(v) = @v = v
  def to_s = "N#{@v}"
end
p "%{a}" % {a: N.new(1)}

class M
  def initialize(v) = @v = v
  def to_s = "M#{@v}"
end
h = {b: M.new(2)}
p "%<b>s" % h
