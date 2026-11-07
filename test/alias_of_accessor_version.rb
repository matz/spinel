# An alias of an attr_reader, attr_writer or attr_accessor method is the
# accessor the class has where the alias is made, as an alias of a def is
# the def: a def of the original name that the class (or a subclass, or the
# accessor's own class) gives later does not take the alias, and the alias
# keeps the accessor's #original_name. The accessor may be the class's own,
# a superclass's or a module's, a reader or a writer, `alias` or
# `alias_method`, and the alias keeps the visibility rules of any method.

# the class's own reader, then its own def of the name
class V
  attr_reader :v
  alias v1 v
  def initialize = @v = 7
  def v = 40
end
o = V.new
p o.v, o.v1
p V.instance_method(:v1).original_name
p V.instance_method(:v1).arity
p V.public_method_defined?(:v1)

# a private reader, a def of its name after the alias
class W
  private
  attr_reader :w
  alias w1 w
  public
  def w = 1
  def initialize = @w = 2
end
p W.private_method_defined?(:w1)

# an accessor's reader and writer, aliased under both spellings
class K
  attr_accessor :x
  alias_method :set_x, :x=
  alias get_x x
end
k = K.new
k.set_x(5)
p k.x, k.get_x
k.x = 6
p k.get_x

# an own writer, then a def of the writer after the alias
class R
  attr_writer :x
  attr_reader :x
  alias_method :set_x, :x=
  def x=(v)
    @x = v * 10
  end
end
r = R.new
r.set_x(3); p r.x
r.x = 3; p r.x

# an inherited reader and writer, the class defining the reader later
class A
  attr_reader :v
  attr_accessor :n
  def initialize = @v = 5
end
class B < A
  alias_method :w, :v
  alias_method :set_n, :n=
  def v = 6
end
p B.new.w, B.new.v
b = B.new
b.set_n(4)
p b.n
p B.instance_method(:w).owner

# a subclass that overrides the name does not take the alias either
class C < A
  alias_method :w, :v
end
class D < C
  def v = 99
end
p D.new.v, D.new.w

# an accessor of a module, aliased in the class that includes it and in the
# module itself; the module's def of the name afterwards
module M
  attr_reader :m
  alias m1 m
end
class E
  include M
  def initialize = @m = :e
  alias m2 m
  def m = :def
end
p E.new.m, E.new.m1, E.new.m2
module M
  def m = :later
end
p E.new.m, E.new.m1, E.new.m2

# through send, method, a symbol proc and respond_to?, and an alias of an alias
class F
  attr_accessor :x
  alias_method :get, :x
  alias_method :put, :x=
  alias got get
  def x = 99
end
f = F.new
f.put(3)
p f.send(:get), f.public_send(:got), f.method(:get).call
p [f].map(&:get)
p f.respond_to?(:get), f.respond_to?(:put)
p f.x
p F.instance_method(:got).original_name

# visibility: private after the alias, a protected accessor
class G
  attr_accessor :x
  alias y x
  alias_method :y=, :x=
  private :x, :x=
  def initialize = @x = 1
end
g = G.new
g.y = 8
p g.y
begin
  g.x
rescue NoMethodError => e
  p e.name
end
p G.private_method_defined?(:x), G.public_method_defined?(:y), G.public_method_defined?(:y=)
class H
  def initialize(n) = @n = n
  def peek(o) = o.m
  protected
  attr_reader :n
  alias m n
end
p H.new(1).peek(H.new(2))
p H.protected_method_defined?(:m)

# a reader and a writer, nil before any write, and a nil written
class N
  attr_reader :v
  attr_writer :v
  alias w v
  alias w= v=
end
n = N.new
p n.w
n.w = [1, 2]
p n.v
n.w = nil
p n.w

# the name from a String, and the alias value of the call
class S
  attr_reader :v
  r = alias_method "w", "v"
  p r
  def v = 2
  def initialize = @v = 1
end
p S.new.v, S.new.w

# an alias made in a required-style reopening, the def in a later one
class T
  attr_reader :v
  def initialize = @v = 1
end
class T
  alias w v
end
class T
  def v = 2
end
p T.new.v, T.new.w

# a reader declared again after the alias, and a writer beside it
class U
  attr_reader :v
  alias w v
  def initialize = @v = 1
  attr_reader :v
  attr_writer :v
end
u = U.new
u.v = 3
p u.v, u.w

# a Method of an alias of an accessor has the accessor's parameter list, not
# a def's: a writer's one parameter has no name; a module's accessor has no
# position
class P
  attr_accessor :x
  alias_method :put, :x=
  alias get x
end
pp = P.new
p P.instance_method(:put).parameters, pp.method(:put).to_proc.parameters
p pp.method(:get).parameters, P.instance_method(:put).arity
p P.instance_method(:put).inspect.sub(/ \S*alias_of_accessor\S*:\d+/, "")
module Q
  attr_reader :q
  alias qq q
end
class P
  include Q
end
p P.instance_method(:qq).source_location
