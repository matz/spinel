# index, rindex and find_index with a value on an Integer, String or Float
# Array answer the position, nil on a miss: a hit, a miss, a nil or boxed
# needle, and the answer used as a number, a Hash key, an Array index, an
# ivar, a method's answer and a nil test.
ia = [3, 1, 4, 1, 5]; sa = %w[x y z y]; fa = [1.5, 2.5, 1.5]
miss = ARGV.size == 0 ? 99 : 4
p ia.index(1), ia.rindex(1), ia.find_index(miss), ia.index(nil)
p sa.index("y"), sa.rindex("y"), sa.index("q"), sa.find_index(nil)
p fa.index(1.5), fa.rindex(1.5), fa.index(9.0), fa.index(1)
bx = [1, "y", 2.5][ARGV.size]
p ia.index(bx), sa.index(bx), fa.index(bx)
i = ia.index(miss)
p i.nil?, (i || -1), i.to_s, i.inspect
j = sa.index("z")
p j + 1, j * 2, ia[j], [j].compact
h = {}
h[ia.index(miss)] = :a
h[ia.index(4)] = :b
p h
class K
  def initialize(a) = @at = a.index(5)
  def at = @at
  def nx(a) = a.find_index(miss_of(a))
  def miss_of(a) = a.max + 1
end
p K.new(ia).at, K.new([1]).at, K.new(ia).nx(ia)
def pos(a, v) = a.index(v)
p pos(ia, 4), pos(ia, 77), pos(sa, "x"), pos(fa, 2.5)
p ("a".."e").to_a.find_index("c"), ("a".."e").to_a.find_index("q")
k = ia.index(miss)
begin
  p k + 1
rescue NoMethodError => e
  puts e.message
end
p [ia.index(4), sa.index("q")].map { |v| v ? v * 10 : :none }
