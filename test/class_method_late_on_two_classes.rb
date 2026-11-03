# Two classes (and two modules) each get a late def of a class-side name.
# A call that runs before a class's own def answers what the class had
# then, as CRuby does, though the other class's def has run: at a load
# point, from a method, a lambda and a block, on a subclass with a late
# def of its own, and for a name no builtin answers.

class A; end
class B; end
def both = "#{A.to_s}+#{B.to_s}"
show = -> { [A.to_s, B.to_s].join(" ") }
puts A.to_s, B.to_s, both, show.()
def A.to_s = "aa"
puts A.to_s, B.to_s, both, show.()
[1].each { puts A.to_s, B.to_s }
def B.to_s = "bb"
puts A.to_s, B.to_s, both, show.()

class P; end
class Q < P; end
puts P.to_s, Q.to_s
def Q.to_s = "qq"
puts P.to_s, Q.to_s
def P.to_s = "pp"
puts P.to_s, Q.to_s

class R; end
class S < R; end
def ins = [R.inspect, S.inspect].join(",")
puts ins
def R.inspect = "ri"
puts ins
def S.inspect = "si"
puts ins

module M; end
module N; end
puts M.to_s, N.to_s
def M.to_s = "mm"
puts M.to_s, N.to_s
def N.to_s = "nn"
puts M.to_s, N.to_s

class U; end
class V; end
puts U.name, V.name
def U.name = "un"
def V.name = "vn"
puts U.name, V.name

class E; end
class F; end
def mk = [(E.make rescue :none), (F.make rescue :none)]
p mk
def E.make = :e
p mk
def F.make = :f
p mk

# a module's late class method beside another class's instance method of
# the name: the entry's arm reaches the module's method only, and the
# instance method, which nothing calls, is not compiled
class K
  def fnd(t) = eval(t)
end
p K.new.class
module G; end
class H
  def initialize(x) = (@v = G.fnd(x))
  attr_reader :v
end
begin
  p H.new(1).v
rescue NoMethodError => e
  p e.message
end
module G
  def self.fnd(x) = x * 2
end
p H.new(3).v
