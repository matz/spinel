# defined?(m::Foo) with m a value (a parameter, a local) answers as the
# read m::Foo finds it: "constant" when m's own chain holds Foo (m, the
# modules it prepends and includes, its superclass), a nested class too;
# Object alone sees the top-level constants and classes; nil otherwise.

module Mx; Foo = 1; end
module Pp; Bar = 2; end
class Base; Baz = 3; class Inner; end; end
class Kid < Base; include Mx; prepend Pp; end
class Lone; end
Top = 4
class TopCls; end

def d(m, n)
  case n
  when :Foo then defined?(m::Foo)
  when :Bar then defined?(m::Bar)
  when :Baz then defined?(m::Baz)
  when :Inner then defined?(m::Inner)
  when :Top then defined?(m::Top)
  when :TopCls then defined?(m::TopCls)
  when :String then defined?(m::String)
  end
end
[Mx, Pp, Base, Kid, Lone, Object].each do |m|
  p [m, %i[Foo Bar Baz Inner Top TopCls String].map { |n| d(m, n) }]
end

# a local that holds one class
k = Kid
p defined?(k::Foo), defined?(k::Nope)

# a constant assigned later: nil before, "constant" after
def late(m) = defined?(m::LATE)
p late(Lone)
class Lone; LATE = 5; end
p late(Lone), late(Base)

# a constant const_set adds at run time
def q(m) = defined?(m::Q)
p q(Base)
Base.const_set(:Q, 1)
p q(Base), q(Kid)

# a private constant is nil through a path
module Hid; Sec = 1; private_constant :Sec; Pub = 2; end
def hid(m) = [defined?(m::Sec), defined?(m::Pub)]
p hid(Hid)

