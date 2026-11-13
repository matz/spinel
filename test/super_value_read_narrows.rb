# spinel: gc-stress
# A `super` reads its method's value only where its own value is used: one
# whose value its place throws away reads nothing, and one ending a method
# reads it only where that method's value is read. Counted wherever it
# stood, the `super(x)` of one class's initialize had the walk ending every
# initialize hand its Array on, and those Arrays stayed boxed. Where a
# super's value is used, through a variable or a method ending in it, the
# Array the super method's last walk answers is handed on and stays as it was.

class Cell
  attr_accessor :v
  def initialize(v = 1.0) = (@v = v)
end

class Base
  def initialize(x) = (@x = x)
  def x = @x
end

# super ends initialize, super with no arguments, super followed by more
class Child < Base
  def initialize(x)
    super(x)
  end
end

class Zchild < Base
  def initialize(x)
    super
  end
end

class Later < Base
  def initialize(x)
    super(x * 2)
    @y = x
  end
  def y = @y
end

# its initialize ends in a walk, whose value nothing reads
class Net
  def initialize(n)
    @a = Array.new(n) { |i| Cell.new(i * 1.0) }
    @b = Array.new(n) { |i| Cell.new(i + 0.5) }
    @b.each { |c| c.v += 1.0 }
  end
  def total = (t = 0.0; @a.each { |c| t += c.v }; @b.each { |c| t += c.v * 2 }; t)
end

# a super whose value is used: the Array the walk answers is handed on
class Shelf
  def initialize(n) = (@items = Array.new(n) { |i| Cell.new(i * 3.0) })
  def items = @items.each { |c| c.v += 0.0 }
end

class Counted < Shelf
  def items = super.size
end

class Passed < Shelf
  def items
    super
  end
end

class Kept < Shelf
  def items
    r = super
    r.map(&:v)
  end
end

p Child.new(1).x, Zchild.new(2).x, Later.new(3).x, Later.new(3).y
p Net.new(4).total
p Counted.new(3).items, Passed.new(2).items.map(&:v), Kept.new(3).items

# many nets, garbage between them
acc = 0.0
30.times do |k|
  acc += Net.new(3 + k % 4).total + Child.new(k).x
  Array.new(50) { |i| "g#{i}" }
end
p acc
