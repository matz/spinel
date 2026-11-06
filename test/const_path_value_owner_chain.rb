# `m::Foo` where m is a value reads the Foo of m's own chain, as CRuby
# does: m itself, the modules m includes, then m's superclass. A class
# whose chain holds no Foo raises NameError, also when one class alone
# holds Foo (its key is bare then) or when Foo is a top-level constant
# (Object's, which CRuby reads only through Object itself).
module X; module Y; end; end
class Z; end
class X::Y::Z; Foo = 1; end
def look(m) = (m::Foo rescue :none)
[X::Y::Z, Z].each { |m| p look(m) }

module Mx; Bar = 7; end
module Wx; Bar = 2; end
module Yx; include Mx; end
class Px; Bar = 3; end
class Kx < Px; end
class Ix; include Mx; end
class Jx < Ix; end
def bar(m) = (m::Bar rescue :none)
p [Mx, Wx, Yx, Px, Kx, Ix, Jx, Z].map { |m| bar(m) }

Top = 5
def top(m) = (m::Top rescue $!.message)
p [Object, Z, Mx].map { |m| top(m) }

# self.class::C in an inherited method: the subclass's own C, else the parent's
class Base
  Name = "base"
  def name = self.class::Name
end
class Sub < Base; end
class Sub2 < Base; Name = "sub2"; end
p [Base.new, Sub.new, Sub2.new].map(&:name)
