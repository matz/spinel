# A method Object holds, its own or a module's, under a name a builtin class
# has too (Array#fill, Array#first, Array#push): main is an Object and no
# Array, so a bare call there runs Object's method.
class Box
  attr_reader :ch
  def initialize(ch) = @ch = ch
end
class Other
  def ch = "o"
end
module Helpers
  def fill(ch) = Box.new(ch)
  def first(n) = n + 1
end
Object.include(Helpers)
class Object
  def push(a) = [a, a]
end

puts fill("-").ch
p first(2)
p push(:a)
p self.first(5)

# the value the bare call answers is of the class the method builds
[Other.new, fill("+")].each { |o| puts o.ch }

class Panel
  def draw = fill("x")
end
[Other.new, Panel.new.draw].each { |o| puts o.ch }

# an Array still runs its own
a = [1, 2, 3]
p a.first(2)
p a.fill(0)
p a.push(4)
