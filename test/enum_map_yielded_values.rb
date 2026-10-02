# map and flat_map hand a user Enumerable's yielded values to their block as
# a block call does, unlike select or to_a, which see them packed: a lone
# `|x|` takes the first value, `|*a|` all of them, and `|x = 9|` its default
# when nothing was yielded. When #each yields one value at some sites and
# several at others, `yield [4, 5]` and `yield 4, 5` pack alike, so `|x|`
# took 4 from the Array; a `for` loop the same.
class Mixed
  include Enumerable
  def each
    yield 1
    yield 2, 3
    yield [4, 5]
    yield
  end
end
m = Mixed.new
p m.map { |x| x }
p m.collect { |x| x }
p m.map { |*a| a }
p m.map { |x = 9| x }
p m.flat_map { |x| [x] }
p m.collect_concat { |*a| [a] }
p m.map { |x, y| [x, y] }
p m.map { |x, | x }
p m.to_a, m.select { |x| x }
r = []
for x in m
  r << x
end
p r
r = []
for x, y in m
  r << [x, y]
end
p r

# every yield gives two values: the packed element is never ambiguous, but
# `|*a|`, `|x = 9|` and flat_map read it as one value
class Pairs
  include Enumerable
  def each
    yield 1, 2
    yield 3, 4
  end
end
q = Pairs.new
p q.map { |x| x }
p q.map { |*a| a }
p q.map { |x = 9| x }
p q.flat_map { |x| [x] }
p q.collect_concat { |*a| [a] }
