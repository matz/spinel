# The class methods of a class made at run time receive the class they are
# called on: self, and the instances a bare new builds there.
def make = Class.new do
  attr_reader :inner
  def initialize(inner = nil) = (@inner = inner)
  def self.build = new
  def self.pair = new(new)
  def self.me = self
  def who = self.class
end

a = make
b = make
p a.me == a, b.me == b, a.me == b
p a.build.class == a, b.build.class == b, a.build.who == a
q = b.pair
p q.class == b, q.inner.class == b
p a.new(a.new).inner.class == a
p [a.me, b.me].uniq.size, [a.build.class, b.build.class] == [a, b]
