# A Struct or Data `initialize` that takes a named `&blk` (and does not
# yield) gets the `.new` site's block, as a plain class's does. The
# constructor took only the other parameters, so its call of initialize
# was one argument short and the C did not compile; the block was dropped.
D = Data.define(:a)
class D
  def initialize(a:, &blk)
    super(a: blk ? blk.call(a) : a)
  end
end
p D.new(a: 1)
p D.new(a: 2) { _1 * 10 }
p D.new(3) { |x| x + 100 }
pr = proc { |x| x - 1 }
p D.new(a: 5, &pr)
p D.new(a: 6, &nil)
class E < D; end
p E.new(a: 7) { _1 * 2 }
p E.new(a: 8)
def build(k) = k.new(a: 9) { _1 * 3 }
p build(D)
p D.new(a: 4).class.new(a: 4) { _1 + 1 }

S = Struct.new(:a, :b) do
  def initialize(a, b = 0, &blk)
    @cb = blk
    super(a, blk ? blk.call(b) : b)
  end
  def again(x) = @cb ? @cb.call(x) : :none
end
s = S.new(1, 2) { "<#{_1}>" }
p s
p s.again(5)
p S.new(3)
p S.new(3).again(1)

K = Struct.new(:a, keyword_init: true) do
  def initialize(a:, &blk)
    super(a: blk ? blk.call(a) : a)
  end
end
p K.new(a: "x") { _1 * 3 }
p K.new(a: "y")

U = Data.define(:a)
class U
  def initialize(a:, &blk) = super(a: a + 1)
end
p U.new(a: 1) { :ignored }
p U.new(a: 1)

class P
  attr_reader :a
  def initialize(a, &blk) = (@a = blk ? blk.call(a) : a)
end
p P.new(1).a, P.new(2) { _1 * 10 }.a
class Q
  attr_reader :a
  def initialize(a:, &blk) = (@a = blk ? blk.call(a) : a)
end
[D, Q].each { |k| p k.new(a: 2) { _1 * 10 }.a }
[D, Q].each { |k| p k.new(a: 3).a }
Y = Data.define(:a)
class Y
  def initialize(a:)
    super(a: block_given? ? yield(a) : a)
  end
end
p Y.new(a: 1) { _1 + 1 }, Y.new(a: 1)
