# A module_function is also its module's private instance method: a module
# that Object includes brings it to every object, reached by a bare call
# at the top level, in another class's method and in a class method, by
# send, and answered by respond_to? only with include_all; a module that
# includes another keeps its own module function in front of the other's.
# A top-level def is Object's own method, in front of what Object includes,
# a module function or a plain instance method.
module Helpers
  module_function

  def double(x) = x * 2
  def label = "helper"
end

class Object
  include Helpers
end

class Report
  def total = double(21)
  def self.title = label
end

p double(4)
p Report.new.total
p Report.title
begin
  1.double(1)
rescue NoMethodError => e
  p e.class
end
p 3.send(:double, 5), Report.new.send(:label)
p [1.respond_to?(:double), 1.respond_to?(:double, true)]
p ["".respond_to?(:label), true.respond_to?(:label), nil.respond_to?(:label)]
p Report.new.respond_to?(:label, true)
p Helpers.double(8)

module Inner
  module_function
  def tag = "inner"
end

module Outer
  include Inner
  module_function
  def tag = "outer"
end

class Box
  include Outer
  def show = tag
end

p Box.new.show, Box.new.send(:tag), Outer.tag, Inner.tag

module Shadowed
  module_function
  def zz = :module_function
end

module Plain
  def yy = :plain
end

class Object
  include Shadowed
  include Plain
end

def zz = :top
def yy = :top

class Caller
  def t = [zz, yy]
end

p [zz, yy], Caller.new.t, [1.send(:zz), 1.send(:yy)], Shadowed.zz
