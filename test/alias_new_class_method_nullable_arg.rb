# A class method aliased as `new` (`alias_method :new, :civil` in
# `class << self`) runs the aliased body: an argument that may be nil
# (an Array element read) reaches its parameters as it does through the
# method's own name.
require "date"

wall = [2026, 11, 14]
p Date.new(wall[0], wall[1], wall[2]).month
y = [2026][0]
p Date.new(y).year
p Date.civil(wall[0], wall[1], wall[2]).day

class Stamp
  attr_reader :year
  def initialize(year) = @year = year
  class << self
    def civil(year = 1970)
      o = allocate
      o.send(:initialize, year)
      o
    end
    alias_method :new, :civil
  end
end
a = [2026, nil]
p Stamp.new(a[0]).year
p Stamp.civil(a[0]).year
p Stamp.new.year
p Stamp.new(a[1]).year

class Tick
  class << self
    def make(n = 1) = n.nil? ? :none : n + 1
    alias new make
  end
end
p Tick.new(a[0])
p Tick.new(a[1])
p Tick.new
p Tick.make(a[0])

class Pair
  def sum(x = 10) = x.nil? ? -1 : x + 1
  alias_method :plus, :sum
  alias add sum
end
q = Pair.allocate
p q.plus(a[0])
p q.add(a[1])
p q.plus
p q.sum(a[0])
