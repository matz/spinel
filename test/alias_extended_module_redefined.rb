# A class-side alias of a method of a module the parent extends keeps the body
# the module has where the alias appears, whatever the module defines later
# (a def or a define_method original); an alias of initialize keeps the
# initialize its point has, too.
module M
  def m = 1
end
module N
  define_method(:o) { :o1 }
end
class A
  extend M
  extend N
end
class B < A
  class << self
    alias n m
    alias p o
  end
end
module M
  def m = 2
end
module N
  def o = :o2
end
p B.n, B.m, B.p, B.o
p B.singleton_methods(false).sort

class C
  def initialize(v) = (@v = v)
  attr_reader :v
end
class D < C
  alias_method :reinit, :initialize
end
class C
  def initialize(v) = (@v = v * 10)
end
d = D.new(1)
d.send(:reinit, 2)
p d.v
