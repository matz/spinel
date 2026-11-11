# A `class << self` alias of an inherited class method keeps the ancestor's
# body in effect where it appears: a later redefinition in the ancestor does
# not take its place.
class P
  def self.m = 1
end
class D < P
  class << self
    alias o m
  end
end
class P
  def self.m = 2
end
p D.o, D.m

# a grandparent's class method, two subclasses aliasing it at different
# points, an alias of an alias
class G0
  def self.m = 1
end
class G1 < G0; end
class G2 < G1
  class << self
    alias n m
  end
end
class G0
  def self.m = 2
end
p G2.n, G2.m, G1.m
class H0
  def self.m = :a
end
class H1 < H0
  class << self
    alias n m
  end
end
class H0
  def self.m = :a2
end
class H2 < H0
  class << self
    alias n m
  end
end
[H1, H2].each { |k| p k.n }
class H1
  class << self
    alias o n
  end
end
p H1.o, H1.m

# the old version reaches the super it had, and self's own helper and state
class S0
  def self.m = :root
end
class S1 < S0
  def self.m = [:a, super]
end
class S2 < S1
  class << self
    alias n m
  end
end
class S1
  def self.m = [:a2, super]
end
p S2.n, S2.m
class T0
  @k = 5
  def self.helper = :h1
  def self.m = [(@k || 0) + 1, helper]
end
class T1 < T0
  @k = 10
  class << self
    alias n m
  end
  def self.helper = :hb
end
class T0
  def self.m = (@k || 0) * 100
end
p T1.n, T1.m, T0.m

# instance and class aliases of the same names do not mix; the singleton
# class lists the alias
class U0
  def self.m = :cm1
  def m = :im1
end
class U1 < U0
  alias n m
  class << self
    alias n m
  end
end
class U0
  def self.m = :cm2
  def m = :im2
end
p U1.n, U1.new.n, U1.m, U1.new.m, U1.singleton_methods(false)

# a define_singleton_method original and a `class << self` define_method one
class V0
  define_singleton_method(:m) { 1 }
end
class V1 < V0
  class << self
    alias n m
  end
end
class V0
  def self.m = 2
end
p V1.n, V1.m
class W0
  class << self
    define_method(:m) { 1 }
  end
end
class W1 < W0
  class << self
    alias n m
  end
end
class W0
  def self.m = 2
end
p W1.n, W1.m

# an alias of a method of its own name keeps the singleton class's list
class X0
  def self.m = 1
end
class X1 < X0
  class << self
    alias_method :m, :m
  end
end
class X0
  def self.m = 2
end
p X1.m, X0.m, X1.singleton_methods(false)
