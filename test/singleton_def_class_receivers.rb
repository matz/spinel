# A `def` whose receiver is self or a class, in parentheses or not, defines a
# class (or main) method, and one on a traced object in parentheses is that
# object's; both stay compiled beside the refusal of a `def`
# on an object no call could reach (test/reject/singleton_untraced_no_self.rb).
class Foo
  def (self).x = 1
  def setup
    def (self.class).made = "m"
  end
  class << self
    def later
      def self.later_made = "l"
    end
  end
  def self.later2
    def self.later2_made = "l2"
  end
end
def (Foo).z = 3
def (self).hi = "hi"
# a class built by Struct.new or Class.new in a constant (calling these is
# refused elsewhere, but the defs themselves compile)
Point = Struct.new(:x)
def Point.origin = "origin"
Kls = Class.new
def Kls.k = "k"

# a traced object in parentheses is the object itself
class Bar; end
bar = Bar.new
def (bar).m = "bar's"
obj = Object.new
def (obj).n = "obj's"

p Foo.x
p bar.m, obj.n
Foo.new.setup
p Foo.made
Foo.later
Foo.later2
p Foo.later_made, Foo.later2_made
p hi
