# Walk#step is a lowered method -- it calls itself with a block, so it
# takes its block as a parameter -- and it is refused. Deferring that
# refusal must not leave the next unit taking itself for a lowered method
# too, where block_given? would ask after a block parameter it does not
# have. The refused statement raises only where it stands: the call with
# a block never reaches it, the call without one does.
# spinel: defer-refusals: 1:3 30 0 1 2
class Walk
  def step(limit, by = 1)
    return proc { "é" }.ruby2_keywords unless block_given?
    return step(limit) { |i| yield i } if by.nil?
    i = 0
    while i < limit
      yield i
      i += by
    end
  end
end

class Bag
  def initialize(items)
    @size = block_given? ? items.sum { |x| yield x } : items.sum
  end
  attr_reader :size
end

p Bag.new([1, 2]).size
p Bag.new([1, 2]) { |x| x * 10 }.size
Walk.new.step(3) { |i| p i }
Walk.new.step(3)
