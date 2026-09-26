# A user class that owns pop or shift must not change what a genuine Array
# does when it reaches the call boxed: its element comes back, and the
# array loses it. Whether the class is ever built or not.
class Bag
  def initialize(items) = @items = items
  def pop = @items.pop
end

h = { nil => [1, 2] }
x = h[nil]
p x.pop
p x

class Queue2
  def initialize(items) = @items = items
  def pop = @items.pop
  def shift = @items.shift
end

g = { nil => [1, 2, 3] }
y = g[nil]
p y.pop
p y.shift
p y
zs = [Queue2.new([7, 8]), [4, 5]]
zs.each { |z| p z.pop }
p zs[1]
w = [[1, 2], Queue2.new([9])][rand(0) > 2 ? 1 : 0]
p w.shift
p w.pop
p w.pop
