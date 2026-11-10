# A reopened builtin's method that hands its block a container, or a boxed
# value, runs through its proc form, and a parameter the call leaves out is
# callee code: its default reads the receiver as self. The call bound the
# receiver as self only on the route that spliced the method, which the
# container excludes, so the fallback rendered the default at the call site
# on the caller's self (`sp_bivar_get(self, ...)` in the main body, which
# has none), and the C did not compile. The call now binds the receiver for
# any left-out default that reads self, whatever route the method takes.
class Array
  def f(a = @v) = yield(a)
  def g(a = first) = yield(a)
  def h(a = size) = yield(a)
  def k(k: size) = yield(k)
  def blk(a = @v, &b) = b.call(a)
  def setv(x) = @v = x
  def me(a = self) = yield(a)
end
p [1].f { |v| v }
p [1, 2].g { |x| x }
p ["a"].g { |x| x }
p %w[x y].g { |x| x + "!" }
p [1.5, 2.5].g { |x| x * 2 }
p [1, "a"].g { |x| x }
p [[1], [2]].g { |x| x }
p [1, 2].h { |x| x }
p [1, "a", :b].h { |x| x }
p [].h { |x| x }
p [1, 2, 3].k { |x| x }
p [1, 2, 3].k(k: 7) { |x| x }
p [1].blk { |x| x }
p [3, 4].me { |x| x }
p [1, 2].g(9) { |x| x }

# the default reads the receiver's own ivar, and nothing else does
a = [1]
a.setv(:recv)
p a.g { |x| x }
p a.f { |x| x }
p [2].f { |x| x }
t = 0
r = [1].f { |v| t += 1; v }
p r, t
p [1].f { |v| [2].f { |w| [v, w] } }
def mk = (puts "mk"; [1])
p mk.f { |x| x }

# in a method, where the main body's self is not the caller's
class Caller
  def run = [1].f { |x| x }
  def run2(s) = s.sf { |v| v }
end
p Caller.new.run

class Hash
  def f(a = @v) = yield(a)
  def h(a = size) = yield(a)
end
p({a: 1}.f { |x| x })
p({a: 1, b: 2}.h { |x| x })
p({}.h { |x| x })

class String
  def f(a = @x) = yield(a)
  def sf(a = @x) = yield(a)
  def two(a = @x, b: size) = yield([a, b])
  def s2(a = @x, b = self) = yield([a, b].size)
end
p "q".f { |v| v }
p Caller.new.run2("q")
p "abc".two { |v| v }
p "zz".s2 { |v| v }

# routes that already worked
p "q".f(5) { |v| v }
class Integer; def g(a = @v) = yield(a); end
p 3.g { |x| x }
class String; def g(a = size) = yield(a); end
p "abc".g { |x| x + 1 }
class Array; def e(a = @x) = yield(a.to_s); end
p [1].e { |v| v }
