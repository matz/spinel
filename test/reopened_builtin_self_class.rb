# `self.class` in a method of a reopened builtin class (Object, Exception) is
# the receiver's own class: the program's classes below it never name it as
# their parent, so it is not one static class. A class method, `new`, `name`
# and `==` through it, a method a module's hook copies into Object, a builtin
# receiver (`new` on a String's, an Array's, a Hash's and an Object's class),
# and the cases that stay one class: a leaf class and a final builtin.
module L
  def self.included(base)
    base.class_eval do
      def note(e) = self.class.record(e)
    end
  end
end
class O
  include L
  def self.record(e) = [:o, e]
  def self.tag = :o
  def initialize = (@v = 1)
end
class Q
  def self.record(e) = [:q, e]
  def self.tag = :q
end
class E < StandardError
  def self.tag = :e
end
class Leaf
  def self.tag = :leaf
  def tg = self.class.tag
  def mk = self.class.new
end
class Integer
  def self.tag = :int
  def itg = self.class.tag
end

class Object
  def note2(e) = self.class.record(e)
  def tg = self.class.tag
  def dup2 = self.class.new
  def nm = self.class.name
  def same?(o) = self.class == o.class
end
class Exception
  def etg = self.class.tag
  def again = self.class.new("x")
end
Object.include(L)

p O.new.note(:b)
p O.new.note2(:b)
p Q.new.note2(:c)
p O.new.tg
p Q.new.tg
p E.new.etg
p E.new("a").again.class
p RuntimeError.new("a").again.class
p O.new.dup2.class
p Object.new.dup2.class
p "s".dup2.class
p [1].dup2.class
p({ a: 1 }.dup2.class)
p O.new.nm
p 1.nm
p O.new.same?(O.new)
p O.new.same?(Q.new)
p Leaf.new.tg
p Leaf.new.mk.class
p 3.itg
