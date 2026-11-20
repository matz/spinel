# find / detect / take_while on a program's Enumerable class are driven
# lazily through an Enumerator, whose emitter took only a literal block: the
# same call given `&pr` was refused at compile time.

class Items
  include Enumerable

  def initialize(a) = @a = a
  def each(&b) = @a.each(&b)
end

pr = proc { |i| i > 1 }
items = Items.new([1, 2, 3])
p items.find(&pr)
p items.detect(&pr)
p items.find(&proc { |i| i > 5 })
p items.take_while(&proc { |i| i < 3 })
p items.find { |i| i > 1 }

big = lambda { |i| i > 2 }
p items.find(&big)

def pick(items, &blk) = items.find(&blk)
p pick(items) { |i| i.even? }
p pick(Items.new(%w[a bb ccc])) { |s| s.size > 1 }

class Pairs
  include Enumerable

  def each
    yield :a, 1
    yield :b, 2
    yield :c, 3
  end
end

p Pairs.new.find(&proc { |_k, v| v > 1 })
p Pairs.new.take_while(&proc { |k| k != :c })
