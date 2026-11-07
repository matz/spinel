# `E.new(*a)` and `super(*a)` for an exception: the splat gives the
# arguments, read as initialize reads them (one message at most).
class A < StandardError; end
def mk(*a) = A.new(*a)
p mk("o").message, mk.message, mk(nil).message, mk(5).message
begin; mk("a", "b"); rescue ArgumentError => e; p e.message; end
p A.new(*[]).message
p A.new("pre", *[]).message
args = ["lit"]
r = A.new(*args); p r.class, r.message
begin; raise A, *args; rescue A => e; p e.message; end
p A.exception(*args).message

class KE < KeyError; end
def ke(*a) = KE.new(*a)
p ke("k").message, ke.message

class WithIv < StandardError
  def initialize(*a); @x = 1; super(*a); end
  def x = @x
end
w = WithIv.new(*["iv"]); p w.message, w.x

class C < StandardError; def initialize(*a) = super(*a); end
p C.new.message, C.new("c").message, C.new(nil).message
begin; C.new(1, 2); rescue ArgumentError => e; p e.message; end
class D < StandardError; def initialize(m, *r) = super(m, *r); end
p D.new("d").message
begin; D.new("d", 2); rescue ArgumentError => e; p e.message; end
class B < StandardError; def initialize(*a) = super(*a, "x"); end
p B.new.message
class KE2 < KeyError; def initialize(*a) = super(*a); end
p KE2.new("k2").message

def std(k, *a) = k == 1 ? ArgumentError.new(*a) : KeyError.new(*a)
p std(1, "x").message, std(2, "y").message, std(2).message
def rt(*a) = RuntimeError.new(*a)
p rt("q").message, rt.message
begin; RuntimeError.new(*[1, 2]); rescue ArgumentError => e; p e.message; end
e = KeyError.new(*["m"], key: :k); p e.message, e.key
n = NameError.new(*["nm", :foo]); p n.message, n.name
p FrozenError.new(*args).message, StopIteration.new(*args).message
p IOError.new(*[]).message, ZeroDivisionError.new(*[:sym]).message
p StandardError.exception(*args).message
class Rx < RuntimeError; end
x = Rx.new(*["rx"]); p x.class, x.message, x.is_a?(RuntimeError)

# keywords beside a splat: Exception#initialize reads them as one more
# argument, their Hash; KeyError's family takes key: and receiver:
begin; A.new(*["m"], x: 1); rescue ArgumentError => e; p e.message; end
begin; A.new(*["m"], receiver: 1); rescue ArgumentError => e; p e.message; end
p A.new(*[], x: 1).message, RuntimeError.new(*[], x: 1).message
full = {k: 1}
p A.new(*[], **full).message
class Kw < StandardError; def initialize(*a, **k) = super(*a, **k); end
begin; Kw.new("m", receiver: 1); rescue ArgumentError => e; p e.message; end
