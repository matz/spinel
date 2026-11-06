# The owner of a Method of a builtin value is the module prepended to its
# class that defines the method, or a reopen's class; an included
# module's method stands behind the class's own.
module Str
  def to_s = "S"
end
module Up
  def upcase = "U"
end
module Cmp
  def between?(a, b) = :cmp
end
class Integer
  prepend Str
end
class String
  prepend Str
  prepend Cmp
  include Up
end
class Object
  def frozen? = :obj
end
p 5.method(:to_s).owner
p 5.method(:to_s).call
p [5.method(:to_s), 1][0].owner
p 5.method(:to_s).unbind.owner
p "a".method(:to_s).owner
p "a".method(:between?).owner
p "a".method(:between?).call(1, 2)
p "a".method(:upcase).owner
p "a".method(:upcase).parameters
p "a".method(:upcase).call
p 5.method(:frozen?).owner
p "a".method(:frozen?).owner
