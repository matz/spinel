# A `...` forwarder no call names, of a name Enumerable calls with a block,
# keeps the block: `each(...)` here is reached only through map, sort and
# include?.
class L
  include Enumerable
  def initialize = @a = [3, 1, 2]
  def each(...) = @a.each(...)
end

p L.new.map { |x| x * 2 }, L.new.sort, L.new.include?(1)
