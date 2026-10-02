# A fresh object rendered or converted where nothing but a C temporary holds
# it -- `p C.new`, `"#{C.new}"`, `puts C.new`, `String(C.new)`,
# `xs.first(C.new)`, a boxed `k ? C.new : 1`, the default inspect's ivar
# walk -- stays alive while its #to_s / #inspect / #to_str / #to_int / #to_a
# allocates. Under SPINEL_GC_STRESS a collection there swept it, and the
# method read whatever reused its slot.
class C
  def initialize = (@v = 7; @w = 9)
  def inspect = (junk; "C(#{@v},#{@w})")
  def to_s = (junk; "S(#{@v},#{@w})")
  def to_str = (junk; "T(#{@v},#{@w})")
  def junk = (a = []; 200.times { |i| a << Pair.new(i, i) }; a.size)
end
class Pair
  def initialize(x, y) = (@v = x; @w = y)
end
class G < C; end
class N
  def initialize = (@v = 2; @w = 9)
  def to_int = (junk; @v)
  def to_a = (junk; [@v, @w])
  def junk = (a = []; 200.times { |i| a << Pair.new(i, i) }; a.size)
end
class D
  def initialize = (@v = 5; @w = 6)
end
class E < D
  def initialize = (super; @z = 1)
end
def mk = C.new

k = rand < 2
p C.new
puts "#{C.new}"
puts C.new
print C.new, "\n"
p mk
puts "#{mk}"
puts(k ? C.new : 1)
p(k ? C.new : 1)
puts "#{k ? C.new : 1}"
puts String(C.new)
puts format("%s|%p", C.new, C.new)
puts G.new
p G.new
puts "#{G.new}"
$stdout.puts C.new
STDOUT.print C.new, "\n"
pp C.new
p [5, 6, 7].first(N.new)
p Integer(N.new)
p Integer(k ? N.new : 1)
p Array(N.new)
puts D.new.inspect.sub(/0x\h+/, "0x")
p [D.new.inspect.sub(/0x\h+/, "0x")]
puts E.new.inspect.sub(/0x\h+/, "0x")
