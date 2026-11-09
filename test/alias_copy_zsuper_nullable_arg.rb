# A bare super in an alias's copy hands on a parameter the copy holds as a
# number or nil to a parent parameter of any value: the nil goes with it.
class Base
  def m(v = nil)
    @b = v if v
  end
  attr_reader :b
end
class T < Base
  def m(v = nil) = super
end
class U < T
  alias r m
end
u = U.new
p u.r, u.r(3), u.b

class FU < T
  alias f m
end
fu = FU.new
p fu.f, fu.f(1.5), fu.b

class IB
  def initialize(v = nil)
    @b = v if v
  end
  attr_reader :b
end
class IT < IB
  def initialize(v = nil) = super
  alias reinit initialize
  public :reinit
end
t = IT.new
p t.reinit(3), t.b
