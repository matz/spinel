# A Method of a module's class method it does not have raises NameError
# naming '#<Class:M>' once a form that makes the module's singleton class
# has run: a def on the singleton, module_function, extend self, an
# instance variable or class variable write (also in a Module.new block,
# also a multiple assignment's), `class << self`, and the reflective
# forms. Before any of them it names 'Module'. Math has its singleton
# class from boot; Comparable has none.
class O
  def self.zz = 0
end
module W
  def self.x = 1
  def b = 1
end
module M
  def y = 1
  def b = 2
  module_function :y
end
module X
  extend self
  def q = 1
end
module Y
  @count = 0
  def b = 1
end
module Z
  class << self
  end
  def b = 1
end
module C1
  @@n = 1
  def b = 1
end
module C2; def b = 1; end
C2.class_variable_set(:@@k, 1)
module C3; def b = 1; end
C3.instance_eval { def x = 1 }
module C4; def b = 1; end
C4.singleton_class
module C5; def b = 1; end
C5.define_singleton_method(:x) { 1 }
module C6
  def b = 1
  private_class_method
end
module C7; def b = 1; end
B1 = Module.new do
  @x = 1
  def b = 1
end
module C8
  @a, @c = 1, 2
  def b = 1
end
module Math; def bb = 1; end
module Comparable; def cc = 1; end
p((C7.method(:b) rescue $!.message))
C7.instance_variable_set(:@a, 1)
p((W.method(:b) rescue $!.message))
p((M.method(:b) rescue $!.message))
p((X.method(:zz) rescue $!.message))
p((Y.method(:b) rescue $!.message))
p((Z.method(:b) rescue $!.message))
p((C1.method(:b) rescue $!.message))
p((C2.method(:b) rescue $!.message))
p((C3.method(:b) rescue $!.message))
p((C4.method(:b) rescue $!.message))
p((C5.method(:b) rescue $!.message))
p((C6.method(:b) rescue $!.message))
p((C7.method(:b) rescue $!.message))
p((B1.method(:b) rescue $!.message))
p((C8.method(:b) rescue $!.message))
p((Math.method(:bb) rescue $!.message))
p((Comparable.method(:cc) rescue $!.message))
