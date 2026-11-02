# `A::B = Class.new do ... end` and `A::M = Module.new do ... end` define
# the class or module at the path write, named after the path.
module A; end
A::B = Class.new { def b = 2 }
p A::B.new.b, A::B.name

module A::N; end
A::N::C = Class.new { def self.x = name; def c = :c }
p A::N::C.x, A::N::C.name, A::N::C.new.c

A::M = Module.new do
  def self.x = name
  def g = 2
end
class Host; include A::M; end
p A::M.x, Host.new.g, Host.include?(A::M)

# The same leaf under two modules makes two classes, from two builders or
# from a header and a builder.
module Z; end
Z::B = Class.new { def b = 3 }
p A::B.new.b, Z::B.new.b, Z::B.name
module Y; end
class Y::H; def h = 4; end
Z::H = Class.new { def h = 5 }
p Y::H.new.h, Z::H.new.h, Z::H.name

# A path builder under another path builder's class, whose leaf another
# module's builder shares.
A::B::D = Module.new { def self.d = name }
Z::B::D = Class.new { def d = 6 }
p A::B::D.d, Z::B::D.new.d, Z::B::D.name

# A namespace that is no class or module of the program (a value) keeps the
# value write: the block runs, then the write raises TypeError and names
# nothing.
V = 1
begin
  V::Q = Class.new { $ran = true; def q = 1 }
rescue TypeError => e
  p e.message, $ran, defined?(Q)
end
A::W = 5
begin
  A::W::R = Module.new { def r = 1 }
rescue TypeError => e
  p e.message, defined?(R)
end

# The block is the class's body: visibility and attribute macros in it
# apply to the class.
A::P = Class.new do
  attr_reader :v
  def initialize(v) = @v = v
  def pub = sec + 1
  def peer?(o) = o.prot == prot
  private
  def sec = 41
  protected
  def prot = :p
end
o = A::P.new(3)
p o.v, o.pub, o.peer?(A::P.new(4))
begin
  o.sec
rescue NoMethodError => e
  p e.class
end
begin
  o.prot
rescue NoMethodError => e
  p e.class
end

# The block's parameter is the class.
A::K = Class.new do |k|
  p k.equal?(self)
  def kk = 1
end
p A::K.new.kk

# The class exists from the write on: defined? and a read ahead of it, also
# in a method called before it, answer as for a value written there.
module E; end
def e_defined = defined?(E::K)
def e_read = E::K
p defined?(E::K), e_defined
begin
  e_read
rescue NameError => e
  p e.message
end
E::K = Class.new { def k = 8 }
p defined?(E::K), e_defined, e_read.new.k

# In its own block the path names nothing yet, also from a method the
# block defines and calls.
A::G = Class.new do
  p defined?(A::G)
  def self.g = 1
  begin
    A::G.g
  rescue NameError
    p :name_error
  end
  def who = defined?(A::G) ? :set : :unset
  p new.who
end
p defined?(A::G), A::G.g, A::G.new.who

# A header reopens a path builder's class; a block-less Class.new under
# another module, beside a builder of the same leaf, is named by its path.
class A::B; def b2 = 7; end
p A::B.new.b2
module X; end
X::B = Class.new
p X::B.name

# A header that names a superclass (Object, the builder's own) reopens the
# class too: the builder stays how it is made, so the reads ahead of the
# write and in its block still find it undefined.
module Rp; end
def rp_defined = defined?(Rp::B)
p rp_defined, defined?(Rp::B)
Rp::B = Class.new do
  p defined?(Rp::B)
  begin
    Rp::B
    p :found
  rescue NameError
    p :name_error
  end
  def self.x = defined?(Rp::B)
end
p defined?(Rp::B), Rp::B.x, rp_defined
class Rp::B < Object
  def c = 3
end
p Rp::B.new.c

# A namespace a path builder is written under is not defined in code that
# runs before its header (BEGIN runs first).
BEGIN { p defined?(A), defined?(A::B) }
