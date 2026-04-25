# Test: reopening a user-defined class merges into the existing one
# instead of producing duplicate C struct/constructor definitions.

class Point
  attr_accessor :x
  def initialize(x, y)
    @x = x
    @y = y
  end
end

class Point
  attr_accessor :y
  def to_s
    "(" + @x.to_s + "," + @y.to_s + ")"
  end
end

p = Point.new(3, 4)
puts p.to_s
puts p.x
puts p.y
p.x = 10
p.y = 20
puts p.to_s

class Foo
end

class Foo
end

puts "ok"
