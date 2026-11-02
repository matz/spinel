# A class method of a reopened builtin stays reachable through a boxed Class
# A call on a value that may be a class reaches that class's class methods,
# and a table such a method takes stays boxed for it. `"x".class` is the
# String class, which the class facts cannot bound (a builtin's), so a value
# that may hold it may reach String.pick even though its other values are
# program classes: String.pick's parameter keeps the box Desk's table is
# passed in, and the boxed Array of mixed values reaches it too.

Item = Struct.new(:id, :w)

class String
  def self.pick(rows) = rows.first
end

class Point
  def self.pick(rows) = rows.last
end

class Desk
  def initialize = @rows = Array.new(2) { |i| Item.new(i, 20 + i) }
  def top = String.pick(@rows)
end

p Desk.new.top.w
k = [Point, "x".class][1 - ARGV.size]
p k.pick([1, "two", 3.0])
