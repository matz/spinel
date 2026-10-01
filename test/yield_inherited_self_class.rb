# An inherited block-taking method that reads self.class, called on a
# subclass instance (#6769): the inlined body bound self to the subclass
# temp and handed it to parent-typed slots without the upcast C needs.
# A class method called on self.class whose body reads the receiving class
# (`name`) must also see the subclass, not the defining class.
class Base
  def run
    puts self.class.name
    yield
  end
end

class Child < Base
end

puts Child.new.run { 1 }

class Shape
  def self.label; "L:" + name; end
  def run
    puts self.class.name
    yield
  end
  def run2
    p self.class
    k = self.class
    puts k.label
    puts self.class.label
    me = self
    puts me.class.name
    yield self.class.name
  end
  def opt
    n = block_given? ? yield : 0
    puts "#{self.class} #{n}"
  end
  def each_x
    [1, 2].each { |i| yield i, self.class.name }
  end
  def to_s_cls; self.class.name; end
end
class Circle < Shape; end
class Disc < Circle
  def run2
    super { |n| puts "g #{n}" }
  end
end
puts Circle.new.run { 1 }
puts Disc.new.run { 2 }
Circle.new.run2 { |n| puts n }
Disc.new.run2
Circle.new.opt
Disc.new.opt { 5 }
Circle.new.each_x { |a, b| puts "#{a} #{b}" }
pr = proc { |n| puts "proc #{n}" }
Circle.new.run2(&pr)
Disc.new.each_x(&pr)
[Shape.new, Circle.new, Disc.new].each { |o| o.run { puts "blk" } }
x = [Circle.new, Disc.new][1]
x.run { 3 }
y = nil
y = Disc.new if ARGV.size == 0
puts y.run { 4 }

module Tagged
  def tag_each
    yield self.class.name
  end
end
class Node
  include Tagged
  attr_reader :v
  def initialize(v = 0); @v = v; end
  def self.make(v); new(v); end
  def self.kind; "kind-" + name.downcase; end
  def dup_with
    o = self.class.make(@v + 1)
    yield o
    o
  end
  def cls_eq(other)
    yield self.class == other.class
  end
  def pair
    yield self, self.class
  end
  def kinds
    yield self.class.kind
  end
  def arr
    [self, self].map { |x| yield x.class }
  end
end
class Leaf < Node; end
class Twig < Leaf
  def self.kind; "twig!"; end
end
c = Leaf.new(1).dup_with { |o| puts "#{o.class} #{o.v}" }
puts c.class
Twig.new(5).dup_with { |o| puts "#{o.class} #{o.v}" }
Leaf.new.cls_eq(Twig.new) { |b| p b }
Twig.new.cls_eq(Twig.new) { |b| p b }
Leaf.new.pair { |a, k| puts "#{a.class} #{k}" }
[Node.new, Leaf.new, Twig.new].each { |o| o.kinds { |s| puts s } }
Twig.new.arr { |k| p k }
Leaf.new.tag_each { |s| puts s }
Twig.new.tag_each { |s| puts s }
[Leaf.new, Twig.new, Node.new].each { |o| o.pair { |a, k| puts k.kind } }
h = { a: Leaf.new }
h[:a].kinds { |s| puts s }
def mk(i) = i == 0 ? Leaf.new : nil
m = mk(0)
m.kinds { |s| puts s } if m
m&.kinds { |s| puts s }
puts Twig.new.kinds(&:upcase)

# the same class-method read without a block
class Plain
  def self.label; "P:" + name; end
  def show; puts self.class.label; end
end
class Fancy < Plain; end
Plain.new.show
Fancy.new.show
