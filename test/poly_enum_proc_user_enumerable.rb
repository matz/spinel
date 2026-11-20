# `x.select(&pr)` where x is known only at run time and holds a program
# object that is Enumerable: the proc-driven helper walked only the builtin
# containers and answered for an empty collection.

class Items
  include Enumerable

  def initialize(a) = @a = a
  def each(&b) = @a.each(&b)
end

pr = proc { |i| i > 1 }
x = [1].size > 3 ? 5 : Items.new([1, 2, 3])
p x.select(&pr)
p x.map(&pr)
p x.reject(&pr)
p x.count(&pr)
p x.find(&pr)
p x.select { |i| i > 1 }

h = { k: Items.new([3, 1]) }
p h[:k].reject(&pr)
p h[:k].filter_map(&proc { |i| i * 2 if i > 1 })

# the elements are what #each yields, not what a #to_a of the class's own
# answers
class Twofold
  include Enumerable
  def each = (yield 1; yield 2; self)
  def to_a = [9]
end

t = [1].size > 3 ? 5 : Twofold.new
p t.to_a
p t.select(&pr), t.map(&pr), t.reject(&pr)
p t.count(&pr), t.find(&pr)
p t.select { |i| i > 1 }, t.reject { |i| i > 1 }

# a Struct has no #each of the program's and walks its members
Pair = Struct.new(:a, :b)
s = [1].size > 3 ? 5 : Pair.new(1, 2)
p s.select(&pr), s.map(&pr)
