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
