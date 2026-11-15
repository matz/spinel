# A top-level include of a module with a yielding method is Object's too:
# any object runs the method with its block, in a class's method as well,
# where the block reads a local of the method around it.
module Loops
  def twice = yield + yield
  def each_pair_of(a, b)
    yield a
    yield b
    self
  end
end
include Loops

p twice { 2 }
p 5.twice { 3 }
p "s".twice { "ab" }
p Object.include?(Loops)

class Plain
  def sum(x) = twice { x }
  def nest = [3, 4].map { |x| twice { x } }
  def gather
    out = []
    each_pair_of(1, 2) { |v| out << v * 10 }
    out
  end
end
p Plain.new.sum(4)
p Plain.new.nest
p Plain.new.gather

def helper = twice { 7 }
p helper
acc = []
[1].each_pair_of(:a, :b) { |v| acc << v }
p acc
