# A fresh receiver of a user #== behind != and of an exception class's own
# #to_s stays alive while the method allocates (GC stress).
class C
  attr_accessor :v
  def initialize = (@v = 7; @w = 9; @s = "s" * 3)
  def ==(o) = (junk; [@v, @w] == o)
  def junk = (a = []; 200.times { |i| a << Pair.new(i, i) }; a.size)
end
class Pair
  def initialize(x, y) = (@v = x; @w = y)
end
class E < StandardError
  def initialize = (super("e"); @v = 3; @w = 4)
  def to_s = (junk; "E(#{@v},#{@w})")
  def junk = (a = []; 200.times { |i| a << Pair.new(i, i) }; a.size)
end
k = [[7, 9], 1][0]
p(C.new != k)
p(C.new != [1, 2])
p E.new.to_s
x = E.new
p x.to_s
c = C.new; c.v = 7; p(c != k)
