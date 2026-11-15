# spinel: gc-stress
# delete, include?, index and count on an @ivar that holds objects of one
# class keep it an unboxed object array, and answer as CRuby does. Each
# compares an element with == as its receiver, so a class with its own ==
# finds an equal object that is not the same one, and delete answers the
# element it took out, the last of the equal ones, or nil. A method called
# on that nil raises NoMethodError. An Array of Integer Arrays takes the
# same calls, and stays boxed.
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
p rows
