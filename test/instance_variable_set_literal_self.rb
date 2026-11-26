# instance_variable_set with a literal name on self: in a value class's
# initialize, and as a statement whose value nobody reads
class V
  def initialize(n)
    instance_variable_set(:@n, n)
    self.instance_variable_set("@m", n * 2)
  end
  def bump(k)
    instance_variable_set(:@n, @n + k)
  end
  def n = @n
  def m = @m
end
v = V.new(41)
p v.n, v.m
v2 = V.new(1)
p v2.instance_variable_set(:@n, 7), v2.n

class P
  attr_reader :rooms
  def initialize(rooms)
    instance_variable_set(:@rooms, rooms)
    nil
  end
  def again(r)
    x = instance_variable_set(:@rooms, r)
    x.size
  end
end
q = P.new(%w[lobby attic])
p q.rooms
p q.again(%w[a b c])
p q.instance_variable_get(:@rooms)

# a value class whose initialize sets its one ivar
class Val
  def initialize(n)
    instance_variable_set(:@n, n)
  end

  def render = @n + 1
end

puts Val.new(41).render

# an Array of Strings set as a statement, the method going on after it
class Rooms
  def initialize(rooms)
    instance_variable_set(:@rooms, rooms)
    nil
  end

  def render = @rooms.join(", ")
end

puts Rooms.new(%w[lobby attic]).render

# a value class whose initialize sets a String ivar, its value made by a
# method that collects
def label(s)
  GC.start
  s + "!"
end

class Named
  def initialize(s)
    instance_variable_set(:@name, label(s))
  end

  def show = @name
end

p Named.new("a").show, Array.new(3) { |i| Named.new(i.to_s).show }
