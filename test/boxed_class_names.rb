# A Class read out of a boxed slot (a mixed container, a method parameter that
# is a Class at one call site and another value at the next) answers
# `subclasses`, `allocate`, `members` and `keyword_init?` as its constant does:
# a boxed user class, a class value read out of `subclasses`, a Struct class
# with and without `keyword_init:`, a class whose instances are value objects
# and a plain class, the String, Array, Hash and Object classes, a boxed Struct
# instance's own `members`, a Class through a method parameter that is a Class
# at one call site and an Integer at the next, and the NoMethodError an Integer
# and a Struct instance raise for the class-side names. A fresh object is bound
# to a local before it is printed: `p` does not root a boxed dispatch's result
# across its inspect.
class Base
  def initialize(v) = @v = v
  attr_reader :v
end
class Kid < Base; end
class Leaf < Kid; end
class Pt
  def initialize(x) = @x = x
  attr_reader :x
end
class V
  def initialize(v = 1) = @v = v
  attr_reader :v
end
S = Struct.new(:a, :b)
K = Struct.new(:a, keyword_init: true)
N = Struct.new(:a, keyword_init: false)

b = [Base, 0][0]
p b.subclasses
p b.subclasses.map(&:name)
p b.subclasses[0].subclasses
p [Leaf, 0][0].subclasses
p b.allocate.class
p b.allocate.v
p [Pt, 0][0].allocate.x
p [V, 0][0].allocate.v
p [V, 0][0].allocate.class
s = [S, 0][0]
p s.members
p s.keyword_init?
p [K, 0][0].keyword_init?
p [N, 0][0].keyword_init?
t = s.allocate
p t
p [S.new(1, 2), 0][0].members
t = [String, 0][0].allocate
p t
t = [Array, 0][0].allocate
p t
t = [Hash, 0][0].allocate
p t
p [Object, 0][0].allocate.class

def kinds(k) = k.is_a?(Class) ? k.subclasses.size : k
p kinds([Base, 0][0])
p kinds([1, 0][0])

[[1, 0][0], [S.new(1, 2), 0][0]].each do |x|
  [:subclasses, :allocate, :keyword_init?].each do |m|
    begin
      case m
      when :subclasses then x.subclasses
      when :allocate then x.allocate
      when :keyword_init? then x.keyword_init?
      end
      puts "no raise"
    rescue NoMethodError => e
      puts e.message
    end
  end
end
