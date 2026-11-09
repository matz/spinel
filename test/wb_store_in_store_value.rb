# spinel: gc-stress
# A store whose value holds stores of its own -- `@seen = Set.new`, with
# Set's initialize inlined into it (`@data = []`) -- has a write barrier on
# each. The inner one had none: the barrier pass rewrote the outer statement
# and went on past it. A rooted new object is promoted on its first
# survival, so when `[]` collected, the Set was old and the young Array it
# then took went unrecorded; the next minor collection freed it while the
# Set still held it (SPINEL_GC_STRESS=2: the mark reached a freed slot).
class Pt
  attr_reader :n
  def initialize(n) = @n = n
end

class Box
  attr_reader :seen
  def initialize
    @seen = Set.new
  end
  def add(v) = @seen.add(v)
end

class Bag
  attr_reader :items
  def initialize = @items = []
end

class Holder
  attr_reader :bag
  def initialize
    @bag = Bag.new
  end
end

bx = Box.new
10.times { |i| bx.add(Pt.new(i)) }
p bx.seen.size, bx.seen.map(&:n).sum
hs = Array.new(5) { Holder.new }
hs.each_with_index { |h, i| (i + 1).times { |k| h.bag.items << "x#{k}" } }
p hs.map { |h| h.bag.items.size }, hs.last.bag.items.last
