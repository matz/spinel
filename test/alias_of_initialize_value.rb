# An alias of initialize is a copy of it under the alias's name, as CRuby's
# alias is a method with initialize's body: it answers initialize's last
# value, where initialize itself is compiled to answer nothing. Private like
# initialize, it raises NoMethodError on an explicit receiver; made public,
# or called bare or through send, it answers that value, also on a receiver
# of several classes and as the writer of an assignment, and a super as its
# last value answers the initialize the super reaches.
class A
  def initialize = @v = 1
  attr_reader :v
  alias reinit initialize
  def again = reinit
end
begin
  A.new.reinit
rescue NoMethodError => e
  puts e.message
end
p A.new.again, A.new.send(:reinit)

class P
  def reinit = 7
end
class Q < P
  def initialize = @v = 1
  alias reinit initialize
  public :reinit
end
[P.new, Q.new].each { |o| x = o.reinit; p x }

class Base
  def initialize(v = 0) = @base = v
  attr_reader :base
end
class W < Base
  alias reinit= initialize
  public :reinit=
end
w = W.new(1)
x = (w.reinit = 5)
p x, w.base

# an initialize with super: the copy's super is initialize's
class S < Base
  def initialize(v = 0)
    super(v * 10)
    @s = v
  end
  attr_reader :s
  alias reset initialize
  public :reset
end
s = S.new(1)
p s.reset(2), s.base, s.s

# a super as the copy's last value answers the initialize it reaches, up a
# chain of supers
class T < Base
  def initialize(v)
    super(v * 2)
  end
  alias reinit initialize
  public :reinit
end
class U < T
  def initialize(v)
    super
  end
  alias again initialize
  public :again
end
t = T.new(1)
p t.reinit(5), t.base
u = U.new(1)
p u.again(3), u.base

# the old one kept under a name and called from a new initialize
class O
  def initialize(v) = @v = v
  attr_reader :v, :w
  alias old_init initialize
  def initialize(v, w = 3)
    r = old_init(v)
    @w = w + r
  end
end
o = O.new(4)
p o.v, o.w
