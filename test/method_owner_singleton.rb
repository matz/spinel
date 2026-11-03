# Method#owner of a class method is its class's singleton class: a
# def self., a class << self method, an inherited one, and a module
# function, which module_function makes the module's own method. A
# module's instance method that `extend self` brings to the module stays
# the module's own
module M
  def a = 1
  module_function :a
  module_function
  def b = 2
end
class C
  def self.f = 3
  def g = 4
  class << self
    def h = 5
  end
end
class D < C
end
module X
  extend self
  def q = 6
end
p M.method(:a).owner
p M.method(:b).owner
p C.method(:f).owner
p C.method(:h).owner
p D.method(:f).owner
p C.new.method(:g).owner
p C.instance_method(:g).owner
m = C.method(:f)
p m.owner
p m.call
p X.method(:q).owner
p X.q
