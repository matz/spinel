# Each def and define_method of a class body takes its class and side from
# the load record: one on a receiver in parentheses, and the defs, the
# `class << self` section, the `private def` and the define_methods of a
# Struct block, whose class is made at its write.
class Foo
  def (self).x = 1
  def self.y = 2
end
def (Foo).z = 3
def (self).hi = "hi"

P = Struct.new(:a) do
  def m = a + 1
  class << self
    def make = new(1)
  end
  private def h = 2
  def uses_h = h * 10
  define_method(:dm) { a * 2 }
  if true
    define_method(:on) { :on }
  end
  %i[r s].each { |v| define_method("get_#{v}") { v } }
end
D = Data.define(:w) do
  def twice = w * 2
end

p Foo.x, Foo.y, Foo.z, hi
p P.make.m, P.new(3).dm, P.new(1).uses_h, P.new(1).on, P.new(1).get_s
p P.private_method_defined?(:h), D.new(4).twice
