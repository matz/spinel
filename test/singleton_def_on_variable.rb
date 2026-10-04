# A singleton method defined on an object held by an instance, class or
# global variable, or built from a local holding a class (an anonymous
# Class.new one too), belongs to that object alone, as on a local or a
# constant: def, define_singleton_method, class << and extend
class K
  attr_accessor :b
end

@a = K.new
other = K.new
def @a.b=(x)
  @b = x
  :v
end
@a.b = 10
p(@a.b ||= 20)
p(@a.b = 5)
p @a.b
p other.respond_to?(:c), @a.class, @a.instance_of?(K)

class Box
  def initialize(v)
    @inner = K.new
    @inner.b = v
    def @inner.hi = "hi #{@b}"
  end

  def say = @inner.hi
end
p Box.new(1).say, Box.new(2).say

klass = Class.new { attr_accessor :b }
obj = klass.new
def obj.b=(x)
  @b = x * 2
end
obj.b = 4
p obj.b

$g = K.new
def $g.c
  @b = 3
end
$g.c
p $g.b

class H
  @@o = K.new
  def self.go
    def @@o.c = (@b = 6)
    @@o.c
    @@o.b
  end
end
p H.go

module Greet
  def greet = "hello #{b}"
end
@e = K.new
@e.b = 4
@e.extend(Greet)
p @e.greet, @e.is_a?(Greet)

@d = K.new
@d.define_singleton_method(:dd) { 7 }
class << @d
  def ee = @b.to_i + 1
end
p @d.dd, @d.ee, @d.singleton_methods.sort
