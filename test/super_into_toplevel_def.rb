# A top-level def is Object's private method, which a super that no program
# class answers reaches ahead of Kernel's: a Kernel function's name, an Array
# subclass's, a yielding def, and a `(...)` forwarder whose calls pass different
# counts. It is behind a core module the class includes (Comparable's clamp,
# through the class or a module), whose method answers the super.

# a method of the program's own at the top level is Object's: a super reaches it ahead of Kernel's
def putc(c) = "P#{c}"
class Topf
  def putc(c) = "<" + super + ">"
end
p Topf.new.putc("x")

# a top-level def a super reaches under an Array subclass, and one that yields
def fmtx(f, n) = "T"
class Ax < Array
  def fmtx(f, n) = "<" + super + ">"
end
p Ax.new([1]).fmtx("%d", 1)
def twice_top = [yield, yield]
class Foo2
  def twice_top = super { 7 }
end
p Foo2.new.twice_top

# a top-level def is behind an included Comparable: its clamp answers the super
class Vv
  include Comparable
  attr_reader :v
  def initialize(v) = @v = v
  def <=>(o) = v <=> o.v
  def clamp(a, b) = super
end
def clamp(a, b) = :top
p Vv.new(5).clamp(Vv.new(1), Vv.new(3)).v

# the same through a module a class includes
module Ord
  include Comparable
end
class V2
  include Ord
  attr_reader :v
  def initialize(v) = @v = v
  def <=>(o) = v <=> o.v
  def clamp(a, b) = super
end
p V2.new(5).clamp(V2.new(1), V2.new(3)).v
# a `(...)` forwarder over a top-level def with an optional, called with different counts
def tfwd(a, b = 2) = a * b
class Foo3
  def tfwd(...) = super
end
p Foo3.new.tfwd(3), Foo3.new.tfwd(3, 4)

# a super reached before the top-level def of its name is made finds none: the plain call raises there too
class Late
  def greet = "L" + super
end
begin
  Late.new.greet
rescue NoMethodError => e
  puts e.class
end
def greet = "T"

# a def in an iterator's block that never runs is none a super finds
[].each { def blk = "T" }
class InBlock
  def blk = "B" + super
end
begin
  InBlock.new.blk
rescue NoMethodError => e
  puts e.class
end

# `include Kernel` is a no-op under Object: the top-level def still answers the super
def fmtk(*a) = "T"
class KI
  include Kernel
  def fmtk(f, n) = "<" + super + ">"
end
p KI.new.fmtk("%d", 1)
