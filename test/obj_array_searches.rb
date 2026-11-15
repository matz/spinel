# spinel: gc-stress
# delete, include?, index, count, delete_if and keep_if on an @ivar that
# holds objects of one class keep it an unboxed object array, and answer
# as CRuby does. Each search compares an element with == as its receiver,
# so a class with its own == finds an equal object that is not the same
# one, and delete answers the element it took out, the last of the equal
# ones, or nil. A method called on that nil raises NoMethodError. An Array
# of Integer Arrays takes the same calls, and stays boxed.
class Wire
  attr_reader :n
  def initialize(n) = @n = n
end

class Server
  def initialize = @wires = []
  def add(w) = @wires << w
  def drop(w) = @wires.delete(w)
  def has?(w) = @wires.include?(w)
  def member(w) = @wires.member?(w)
  def at(w) = @wires.index(w)
  def find_at(w) = @wires.find_index(w)
  def tally(w) = @wires.count(w)
  def size = @wires.size
  def ns = @wires.map(&:n).join(",")
  def drop_odd
    @wires.delete_if { |w| w.n.odd? }
    nil
  end
  def keep_big
    @wires.keep_if { |w| w.n > 2 }
    nil
  end
end

s = Server.new
a = Wire.new(1)
b = Wire.new(2)
s.add(a); s.add(b); s.add(a); s.add(Wire.new(3))
p s.has?(a), s.has?(Wire.new(1)), s.member(b), s.has?(nil), s.has?(1)
p s.at(b), s.at(Wire.new(2)), s.find_at(a)
p s.tally(a), s.tally(Wire.new(9))
puts s.drop(a).n
p s.drop(a), s.drop(nil), s.drop(5)
puts s.ns
begin
  puts s.drop(Wire.new(2)).n
rescue NoMethodError => e
  puts e.message
end
begin
  puts s.at(a) + 1
rescue NoMethodError => e
  puts e.message
end
s.add(Wire.new(5)); s.add(Wire.new(4))
s.drop_odd
puts s.ns
s.keep_big
puts s.ns

# A class with its own ==, which the searches call with the element as its
# receiver, once per element, in order.
class Point
  attr_reader :x, :y
  def initialize(x, y) = (@x = x; @y = y)
  def ==(o)
    puts "== #{x},#{y}"
    o.is_a?(Point) && x == o.x && y == o.y
  end
end

class Path
  def initialize = @points = []
  def add(pt) = @points << pt
  def drop(pt) = @points.delete(pt)
  def has?(pt) = @points.include?(pt)
  def at(pt) = @points.index(pt)
  def tally(pt) = @points.count(pt)
  def xs = @points.map(&:x).join(",")
end

path = Path.new
first = Point.new(1, 1)
path.add(first); path.add(Point.new(2, 2)); path.add(Point.new(1, 1))
p path.has?(Point.new(2, 2))
p path.has?(first)
p path.at(Point.new(1, 1))
p path.tally(Point.new(1, 1))
gone = path.drop(Point.new(1, 1))
p gone.equal?(first), gone.x
puts path.xs
p path.drop(Point.new(7, 7))

rows = [[1, 2], [3, 4], [1, 2]]
p rows.include?([3, 4]), rows.index([1, 2]), rows.count([1, 2])
p rows.delete([1, 2])
rows.delete_if { |r| r[0] == 3 }
p rows

# A frozen object array: delete raises FrozenError when it finds an equal
# element, and leaves the array as it was; a miss answers nil.
WIRES = [a, b].freeze
begin
  WIRES.delete(a)
rescue FrozenError => e
  puts e.class
end
p WIRES.size, WIRES.delete(Wire.new(9))

# index and find_index on a constant object array answer nil for a
# missing element.
i = WIRES.index(Wire.new(9))
p i, WIRES.index(b), WIRES.find_index(a)
j = WIRES.find_index(Wire.new(9))
p j

# An == that stores into the array: delete keeps what it stored.
class Swap
  attr_reader :n
  def initialize(n, arr) = (@n = n; @arr = arr)
  def ==(o)
    @arr[0] = Swap.new(99, @arr) if n == 1
    false
  end
end

class Swaps
  def initialize = @all = []
  def add(s) = @all << s
  def drop(s) = @all.delete(s)
  def all = @all
  def first_n = @all[0].n
end

sw = Swaps.new
sw.add(Swap.new(1, sw.all))
p sw.drop(Swap.new(5, sw.all)), sw.first_n
