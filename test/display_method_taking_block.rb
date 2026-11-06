# A #to_s or #inspect that takes a block, by &block or by yield, answers
# for a boxed receiver too: the dispatch declared the first without its
# block parameter (conflicting types for sp_X_to_s), and called a function
# the second never has (it is inlined; only its proc-form clone exists).
class Tagged
  def initialize(v)
    @v = v
  end

  def to_s(&block)
    block ? block.call(@v) : "tag#{@v}"
  end

  def inspect(&block)
    block ? block.call(@v) : "#<Tagged #{@v}>"
  end
end

class Yielding
  def initialize(v)
    @v = v
  end

  def to_s
    block_given? ? yield(@v) : "yield#{@v}"
  end

  def inspect
    block_given? ? yield(@v) : "#<Yielding #{@v}>"
  end
end

items = [1, Tagged.new(2), Yielding.new(3)]
p items.map(&:to_s)
p items
puts items
items.each { |x| puts "<#{x}>" }
p Tagged.new(4).to_s { |v| "block#{v}" }
p Yielding.new(5).to_s { |v| "block#{v}" }
p({ a: Yielding.new(6), b: Tagged.new(7) })
