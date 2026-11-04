# A class whose name is long (here 125 characters, namespaced) compiles.
# The emitters build casts and calls of a class's C name in fixed buffers
# (the boxed #to_int / #to_f / #deconstruct / #to_a dispatchers, the
# bind_call and Method arms, ...), and a long name cut the text short, so
# the C did not compile. A C name past 40 characters is now its first
# characters and a hash of the whole name.
module OuterLongLongLongLongLongLongLongLongLongLong
  class InnerLongLongLongLongLongLongLongLongLongLongLongLongLongLongLongLongLongLongLongLongLongLongLongLongLongLongLongLongLongLong
    include Enumerable
    include Comparable
    attr_accessor :v
    def initialize(v) = (@v = v)
    def to_int = @v
    def to_s = "K(#{@v})"
    def to_ary = [@v, -@v]
    def deconstruct = [:d, @v]
    def each = (yield @v; yield @v * 10)
    def add(a, b = @v) = a + b
    def <=>(x) = v <=> x.v
    def deconstruct_keys(ks) = {v: @v}
  end
  class Num < Numeric
    def initialize(f) = (@f = f)
    def to_f = @f
  end
  class InnerLongLongLongLongLongLongLongLongLongLongLongLongLongLongLongLongLongLongLongLongLongLongLongLongLongLongLongLongLongLongPair
    def initialize(v) = (@v = v)
    def to_a = [@v, @v + 1]
  end
  Row = Struct.new(:a, :b)
  InnerLongLongLongLongLongLongLongLongLongLongLongLongLongLongLongLongLongLongLongLongLongLongLongLongLongLongLongLongLongLongRow = Struct.new(:a, :b)
  class InnerLongLongLongLongLongLongLongLongLongLongLongLongLongLongLongLongLongLongLongLongLongLongLongLongLongLongLongLongLongLongError < StandardError
    attr_accessor :code
  end
  class Bag
    attr_reader :to_a
    def initialize = (@to_a = [7, 8])
  end
end
K = OuterLongLongLongLongLongLongLongLongLongLong::InnerLongLongLongLongLongLongLongLongLongLongLongLongLongLongLongLongLongLongLongLongLongLongLongLongLongLongLongLongLongLong
xs = [K.new(3), 1, "s", nil]
o = xs[ARGV.size]
p Integer(o)                         # the Kernel#Integer bridge
p [10, 20, 30, 40].fetch(o)          # #to_int
p Math.sqrt([OuterLongLongLongLongLongLongLongLongLongLong::Num.new(16.0), 1][ARGV.size])   # the #to_f row of a Numeric
case o                               # the boxed #deconstruct
in [:d, n] then p n
end
p [*[OuterLongLongLongLongLongLongLongLongLongLong::InnerLongLongLongLongLongLongLongLongLongLongLongLongLongLongLongLongLongLongLongLongLongLongLongLongLongLongLongLongLongLongPair.new(3), 1][ARGV.size]]   # #to_a
a, b = [o, 0][ARGV.size]             # #to_ary
p [a, b]
p o.first, o.map { |e| e + 1 }       # #each through Enumerable
p o.select(&:odd?)
p [*[OuterLongLongLongLongLongLongLongLongLongLong::Bag.new, 1][ARGV.size]]   # a reader answering #to_a
r = [OuterLongLongLongLongLongLongLongLongLongLong::Row.new(1, 2), 1][ARGV.size]
case r
in [x, y] then p x + y
end
p K.instance_method(:v).bind_call(o)          # bind_call on a boxed value
p K.instance_method(:add).bind_call(K.new(6), 1)   # its default reads self
def via(c, obj) = c.instance_method(:v).bind_call(obj)
p via(K, K.new(5))                            # on a Class value
m = K.new(4).method(:add)
p m.call(1), m.call(1, 1)                     # a Method whose default reads self
p o.method(:add).call(2)
# Ruby sees the whole name; only the C name is cut
p o.class.name.size, o.class.name.end_with?("LongLong")
p o.inspect.start_with?("#<#{K.name}:")
begin; o.nope; rescue NoMethodError => e; p e.message.include?(K.name); end
# a long-named Struct's #to_a, #inspect and #to_s
lr = [OuterLongLongLongLongLongLongLongLongLongLong::InnerLongLongLongLongLongLongLongLongLongLongLongLongLongLongLongLongLongLongLongLongLongLongLongLongLongLongLongLongLongLongRow.new(1, 2), 1][ARGV.size]
p lr.to_a, lr.inspect
puts lr.to_s
p OuterLongLongLongLongLongLongLongLongLongLong::InnerLongLongLongLongLongLongLongLongLongLongLongLongLongLongLongLongLongLongLongLongLongLongLongLongLongLongLongLongLongLongRow.new(3, 4).to_a
p o.between?(K.new(1), K.new(5)), (o <=> K.new(9))   # Comparable on a boxed value
case o
in {v:} then p v                    # #deconstruct_keys
end
$g = xs[ARGV.size]
$g.v += 1                            # an attribute op-assign on a global
p $g.v
begin
  ex = OuterLongLongLongLongLongLongLongLongLongLong::InnerLongLongLongLongLongLongLongLongLongLongLongLongLongLongLongLongLongLongLongLongLongLongLongLongLongLongLongLongLongLongError.new("bad"); ex.code = 7; raise ex
rescue OuterLongLongLongLongLongLongLongLongLongLong::InnerLongLongLongLongLongLongLongLongLongLongLongLongLongLongLongLongLongLongLongLongLongLongLongLongLongLongLongLongLongLongError => err          # the rescue binds the class's own struct
  p err.message, err.code
end

# a long name whose 31-byte cut falls inside a character keeps whole ones
class Axxxxxxxxxxxxxxxxxxxxxxxxxxxxxéyyyyyyyyyyyyyyyyyyyy
  def hi = :utf8
end
p Axxxxxxxxxxxxxxxxxxxxxxxxxxxxxéyyyyyyyyyyyyyyyyyyyy.new.hi, Axxxxxxxxxxxxxxxxxxxxxxxxxxxxxéyyyyyyyyyyyyyyyyyyyy.name.size
# a short class named exactly what a long class's C name became, defined
# after it and before it: the two still get distinct C names
class Latexxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx
  def hi = :late_long
end
class Latexxxxxxxxxxxxxxxxxxxxxxxxxxx_aaa7c339
  def hi = :late_short
end
class Earlyxxxxxxxxxxxxxxxxxxxxxxxxxx_1097379c
  def hi = :early_short
end
class Earlyxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx
  def hi = :early_long
end
p [Latexxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx.new, 1][ARGV.size].hi, [Latexxxxxxxxxxxxxxxxxxxxxxxxxxx_aaa7c339.new, 1][ARGV.size].hi
p [Earlyxxxxxxxxxxxxxxxxxxxxxxxxxx_1097379c.new, 1][ARGV.size].hi, [Earlyxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx.new, 1][ARGV.size].hi
